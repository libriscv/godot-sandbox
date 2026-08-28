#include "source_model.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace gdscript {
namespace {

constexpr uint32_t MAGIC = 0x4d445347u; // "GSDM"
constexpr uint16_t MAJOR = 1;
constexpr uint16_t MINOR = 0;
constexpr size_t MAX_BLOB = 32u * 1024u * 1024u;
constexpr uint32_t MAX_RECORDS = 100000;
constexpr uint32_t MAX_STRING = 1024u * 1024u;
constexpr uint32_t MAX_DIAGNOSTICS = 100;

enum Section : uint32_t { META = 1, DIAGNOSTICS = 2, DECLARATIONS = 3,
	PROPERTIES = 4, CARET = 5, SAFE_LINES = 6 };

template <typename T> void scalar(std::vector<uint8_t> &out, T value) {
	static_assert(std::is_trivially_copyable<T>::value, "raw bytes only");
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}
void string(std::vector<uint8_t> &out, const std::string &value) {
	scalar<uint32_t>(out, uint32_t(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}
void range(std::vector<uint8_t> &out, const SourceRange &value) {
	scalar<uint32_t>(out, value.start_line); scalar<uint32_t>(out, value.start_column);
	scalar<uint32_t>(out, value.end_line); scalar<uint32_t>(out, value.end_column);
}
void section(std::vector<uint8_t> &out, uint32_t id, const std::vector<uint8_t> &body) {
	scalar<uint32_t>(out, id); scalar<uint32_t>(out, uint32_t(body.size()));
	out.insert(out.end(), body.begin(), body.end());
}

struct Reader {
	const uint8_t *data = nullptr; size_t size = 0; size_t at = 0; bool ok = true;
	template <typename T> T scalar() {
		T value{};
		if (!ok || at > size || sizeof(T) > size - at) { ok = false; return value; }
		std::memcpy(&value, data + at, sizeof(T)); at += sizeof(T); return value;
	}
	std::string string() {
		const uint32_t n = scalar<uint32_t>();
		if (!ok || n > MAX_STRING || at > size || n > size - at) { ok = false; return {}; }
		std::string value(reinterpret_cast<const char *>(data + at), n); at += n; return value;
	}
	SourceRange range() {
		return {scalar<uint32_t>(), scalar<uint32_t>(), scalar<uint32_t>(), scalar<uint32_t>()};
	}
};

bool valid_range(const SourceRange &r) {
	return r.start_line <= r.end_line &&
		(r.start_line != r.end_line || r.start_column <= r.end_column);
}

std::string trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
	return s.substr(a, b - a);
}
bool word_start(const std::string &s, const char *word) {
	const size_t n = std::strlen(word);
	return s.compare(0, n, word) == 0 && (s.size() == n || std::isspace(
			static_cast<unsigned char>(s[n])) || s[n] == '(');
}
std::string identifier(const std::string &s, size_t at) {
	while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) at++;
	const size_t begin = at;
	while (at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_')) at++;
	return s.substr(begin, at - begin);
}
uint32_t indentation(const std::string &s) {
	uint32_t n = 0;
	for (char c : s) { if (c == ' ') n++; else if (c == '\t') n += 4; else break; }
	return n;
}
std::vector<std::string> split_lines(const std::string &source) {
	std::vector<std::string> lines;
	std::stringstream stream(source); std::string line;
	while (std::getline(stream, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); lines.push_back(line); }
	if (source.empty() || (!source.empty() && source.back() == '\n')) lines.push_back({});
	return lines;
}
void diagnostic(SourceModel &model, const std::string &code, const std::string &message,
		uint32_t line, uint32_t column) {
	if (model.diagnostics.size() >= MAX_DIAGNOSTICS) return;
	model.diagnostics.push_back({DiagnosticSeverity::ERROR, code, message, model.path,
		{line, column, line, column + 1}});
}

void warning(SourceModel &model, const std::string &code, const std::string &message,
		uint32_t line, uint32_t column) {
	if (model.diagnostics.size() >= MAX_DIAGNOSTICS) return;
	model.diagnostics.push_back({DiagnosticSeverity::WARNING, code, message, model.path,
		{line, column, line, column + 1}});
}

size_t word_occurrences(const std::string &source, const std::string &word) {
	size_t count = 0;
	for (size_t at = source.find(word); at != std::string::npos; at = source.find(word, at + word.size())) {
		const bool left = at == 0 || !(std::isalnum(static_cast<unsigned char>(source[at - 1])) || source[at - 1] == '_');
		const size_t end = at + word.size();
		const bool right = end == source.size() || !(std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_');
		if (left && right) count++;
	}
	return count;
}

// One physical line with its comments removed, plus whether the lines above it
// left a delimiter or a backslash open. Diagnostics are per statement, not per
// line: a newline inside (), [] or {} continues the statement above it.
struct LineScan {
	std::string code;
	bool continuation = false;
	bool unterminated = false;
	int closing_underflow = 0;
};

struct SourceScan {
	std::vector<LineScan> lines;
	std::vector<uint32_t> unclosed_lines;
	uint32_t unterminated_block_string = 0;
};

// A declaration keyword cannot appear inside brackets, so one at the start of a
// line ends an unclosed delimiter instead of letting it swallow the rest of the
// file. Editor analysis is error-tolerant; the compiler proper still refuses.
bool resumes_after_unclosed(const std::string &text) {
	static const char *const keywords[] = {"func", "static", "var", "const",
		"class", "class_name", "extends", "signal", "enum"};
	for (const char *word : keywords) if (word_start(text, word)) return true;
	return false;
}

// A line opening with a statement keyword has an effect even when it carries no
// assignment and no call, so it is never a standalone expression.
bool statement_keyword(const std::string &text) {
	static const char *const keywords[] = {"return", "pass", "break", "continue",
		"breakpoint", "await", "assert", "if", "elif", "else", "for", "while",
		"match", "when", "super", "extends", "class_name", "static", "signal",
		"var", "const", "func", "class", "enum"};
	for (const char *word : keywords) if (word_start(text, word)) return true;
	return false;
}

SourceScan scan_lines(const std::vector<std::string> &lines) {
	SourceScan scan;
	scan.lines.resize(lines.size());
	std::vector<std::pair<char, uint32_t>> open; // delimiter kind and its line
	char block_quote = 0;
	uint32_t block_line = 0;
	bool backslash = false;
	for (size_t i = 0; i < lines.size(); i++) {
		LineScan &out = scan.lines[i];
		const std::string &raw = lines[i];
		if (!open.empty() && block_quote == 0 && resumes_after_unclosed(trim(raw))) {
			scan.unclosed_lines.push_back(open.front().second);
			open.clear();
		}
		out.continuation = backslash || !open.empty() || block_quote != 0;
		backslash = false;
		std::string code;
		for (size_t at = 0; at < raw.size(); at++) {
			const char c = raw[at];
			if (block_quote != 0) {
				if (c == block_quote && at + 2 < raw.size() && raw[at + 1] == block_quote &&
						raw[at + 2] == block_quote) { block_quote = 0; at += 2; }
				continue;
			}
			if (c == '#') break;
			if (c == '"' || c == '\'') {
				if (at + 2 < raw.size() && raw[at + 1] == c && raw[at + 2] == c) {
					// A block string stands in for its own contents, so the statement
					// holding it still reads as complete.
					block_quote = c; block_line = uint32_t(i + 1); at += 2;
					code += c; code += c; continue;
				}
				code += c;
				bool closed = false;
				for (at++; at < raw.size(); at++) {
					code += raw[at];
					if (raw[at] == '\\') { if (at + 1 < raw.size()) code += raw[++at]; continue; }
					if (raw[at] == c) { closed = true; break; }
				}
				if (!closed) out.unterminated = true;
				continue;
			}
			if (c == '(' || c == '[' || c == '{') {
				open.push_back({c, uint32_t(i + 1)});
			} else if (c == ')' || c == ']' || c == '}') {
				const char match = c == ')' ? '(' : (c == ']' ? '[' : '{');
				if (open.empty() || open.back().first != match) out.closing_underflow++;
				else open.pop_back();
			}
			code += c;
		}
		code = trim(code);
		if (!code.empty() && code.back() == '\\') { code.pop_back(); code = trim(code); backslash = true; }
		out.code = std::move(code);
	}
	if (!open.empty()) scan.unclosed_lines.push_back(open.front().second);
	if (block_quote != 0) scan.unterminated_block_string = block_line;
	return scan;
}

} // namespace

std::vector<uint8_t> encode_source_model(const SourceModel &model) {
	std::vector<uint8_t> out;
	scalar<uint32_t>(out, MAGIC); scalar<uint16_t>(out, MAJOR); scalar<uint16_t>(out, MINOR);
	scalar<uint32_t>(out, 6);
	std::vector<uint8_t> body;
	string(body, model.path); section(out, META, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.diagnostics.size()));
	for (const auto &d : model.diagnostics) {
		scalar<uint8_t>(body, uint8_t(d.severity)); string(body, d.code); string(body, d.message);
		string(body, d.path); range(body, d.range);
	}
	section(out, DIAGNOSTICS, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.declarations.size()));
	for (const auto &d : model.declarations) {
		scalar<uint8_t>(body, uint8_t(d.kind)); string(body, d.name); string(body, d.declared_type);
		string(body, d.resolved_type); range(body, d.declaration); range(body, d.lexical_scope);
		scalar<int32_t>(body, d.parent); scalar<uint32_t>(body, d.flags); string(body, d.documentation);
		scalar<uint32_t>(body, uint32_t(d.children.size())); for (int32_t v : d.children) scalar<int32_t>(body, v);
		scalar<uint32_t>(body, uint32_t(d.parameters.size()));
		for (const auto &p : d.parameters) { string(body, p.name); string(body, p.declared_type); string(body, p.default_text); range(body, p.declaration); }
		string(body, d.return_type);
		scalar<uint32_t>(body, uint32_t(d.enum_members.size()));
		for (const auto &e : d.enum_members) { string(body, e.name); scalar<int64_t>(body, e.value); range(body, e.declaration); }
		scalar<uint32_t>(body, uint32_t(d.annotation_arguments.size())); for (const auto &a : d.annotation_arguments) string(body, a);
	}
	section(out, DECLARATIONS, body); body.clear();
	const std::vector<uint8_t> props = encode_property_signatures(model.properties);
	body.insert(body.end(), props.begin(), props.end()); section(out, PROPERTIES, body); body.clear();
	scalar<uint8_t>(body, uint8_t(model.caret.kind)); scalar<int32_t>(body, model.caret.declaration);
	string(body, model.caret.receiver_text); string(body, model.caret.receiver_type); string(body, model.caret.callee);
	scalar<int32_t>(body, model.caret.argument_index); section(out, CARET, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.safe_lines.size())); for (uint32_t line : model.safe_lines) scalar<uint32_t>(body, line);
	section(out, SAFE_LINES, body);
	return out;
}

bool decode_source_model(const uint8_t *data, size_t size, SourceModel &out) {
	out = SourceModel{};
	if (data == nullptr || size > MAX_BLOB) return false;
	Reader reader{data, size};
	if (reader.scalar<uint32_t>() != MAGIC || reader.scalar<uint16_t>() != MAJOR) return false;
	reader.scalar<uint16_t>();
	const uint32_t section_count = reader.scalar<uint32_t>();
	if (!reader.ok || section_count > 256) return false;
	SourceModel staged;
	for (uint32_t s = 0; s < section_count; s++) {
		const uint32_t id = reader.scalar<uint32_t>(); const uint32_t length = reader.scalar<uint32_t>();
		if (!reader.ok || reader.at > size || length > size - reader.at) return false;
		Reader part{reader.data + reader.at, length}; reader.at += length;
		if (id == META) {
			staged.path = part.string();
		}
		else if (id == DIAGNOSTICS) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return false;
			for (uint32_t i = 0; i < count; i++) {
				SourceDiagnostic d; const uint8_t severity = part.scalar<uint8_t>();
				if (severity > uint8_t(DiagnosticSeverity::INFO)) return false;
				d.severity = DiagnosticSeverity(severity); d.code = part.string(); d.message = part.string(); d.path = part.string(); d.range = part.range();
				if (!valid_range(d.range)) return false;
				staged.diagnostics.push_back(std::move(d));
			}
		} else if (id == DECLARATIONS) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return false;
			for (uint32_t i = 0; i < count; i++) {
				SourceDeclaration d; const uint8_t kind = part.scalar<uint8_t>(); if (kind > uint8_t(DeclarationKind::ANNOTATION)) return false;
				d.kind = DeclarationKind(kind); d.name = part.string(); d.declared_type = part.string(); d.resolved_type = part.string(); d.declaration = part.range(); d.lexical_scope = part.range(); d.parent = part.scalar<int32_t>(); d.flags = part.scalar<uint32_t>(); d.documentation = part.string();
				uint32_t n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return false; for (uint32_t j = 0; j < n; j++) d.children.push_back(part.scalar<int32_t>());
				n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return false; for (uint32_t j = 0; j < n; j++) { SourceParameter p; p.name = part.string(); p.declared_type = part.string(); p.default_text = part.string(); p.declaration = part.range(); d.parameters.push_back(std::move(p)); }
				d.return_type = part.string(); n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return false;
				for (uint32_t j = 0; j < n; j++) { SourceEnumMember e; e.name = part.string(); e.value = part.scalar<int64_t>(); e.declaration = part.range(); d.enum_members.push_back(std::move(e)); }
				n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return false; for (uint32_t j = 0; j < n; j++) d.annotation_arguments.push_back(part.string());
				if (!valid_range(d.declaration) || !valid_range(d.lexical_scope)) return false;
				staged.declarations.push_back(std::move(d));
			}
		} else if (id == PROPERTIES) {
			if (!decode_property_signatures(part.data, part.size, staged.properties)) return false;
			part.at = part.size;
		} else if (id == CARET) {
			const uint8_t kind = part.scalar<uint8_t>(); if (kind > uint8_t(CaretKind::STRING_NAME)) return false;
			staged.caret.kind = CaretKind(kind); staged.caret.declaration = part.scalar<int32_t>(); staged.caret.receiver_text = part.string(); staged.caret.receiver_type = part.string(); staged.caret.callee = part.string(); staged.caret.argument_index = part.scalar<int32_t>();
		} else if (id == SAFE_LINES) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return false;
			for (uint32_t i = 0; i < count; i++) staged.safe_lines.push_back(part.scalar<uint32_t>());
		} else { part.at = part.size; }
		if (!part.ok || part.at != part.size) return false;
	}
	if (!reader.ok || reader.at != size) return false;
	for (size_t i = 0; i < staged.declarations.size(); i++) {
		const auto &d = staged.declarations[i];
		if (d.parent >= int32_t(staged.declarations.size())) return false;
		int depth = 0; int32_t parent = d.parent;
		while (parent >= 0 && depth++ <= 64) { parent = staged.declarations[size_t(parent)].parent; }
		if (depth > 64) return false;
		for (int32_t child : d.children) if (child < 0 || child >= int32_t(staged.declarations.size())) return false;
	}
	if (staged.caret.declaration >= int32_t(staged.declarations.size())) return false;
	out = std::move(staged); return true;
}

SourceModel analyze_source(const std::string &source, const std::string &path,
		uint32_t flags, int32_t caret_line, int32_t caret_column) {
	SourceModel model; model.path = path;
	const std::vector<std::string> lines = split_lines(source);
	const SourceScan scan = scan_lines(lines);
	// One entry per physical line, non-empty only where a statement begins; the
	// continuation lines it swallowed are joined into it.
	std::vector<std::string> statements(lines.size());
	for (size_t i = 0; i < lines.size(); i++) {
		if (scan.lines[i].continuation || scan.lines[i].code.empty()) continue;
		std::string text = scan.lines[i].code;
		for (size_t j = i + 1; j < lines.size() && scan.lines[j].continuation; j++) {
			if (scan.lines[j].code.empty()) continue;
			text += ' '; text += scan.lines[j].code;
		}
		statements[i] = std::move(text);
	}
	if ((flags & ANALYZE_DIAGNOSTICS) != 0) {
		for (size_t i = 0; i < lines.size(); i++) {
			if (scan.lines[i].unterminated) diagnostic(model, "UNTERMINATED_STRING",
					"Unterminated string literal", uint32_t(i + 1), uint32_t(trim(lines[i]).size()));
		}
		if (scan.unterminated_block_string != 0) diagnostic(model, "UNTERMINATED_STRING",
				"Unterminated string literal", scan.unterminated_block_string, 1);
		for (const uint32_t line : scan.unclosed_lines) diagnostic(model, "EXPECTED_CLOSING_DELIMITER",
				"Expected a closing delimiter", line, uint32_t(trim(lines[size_t(line - 1)]).size()));
	}
	std::vector<std::pair<uint32_t, int32_t>> scopes;
	std::string pending_doc;
	for (size_t i = 0; i < lines.size(); i++) {
		const uint32_t line_no = uint32_t(i + 1); const std::string &raw = lines[i];
		const uint32_t indent = indentation(raw); const std::string raw_text = trim(raw);
		if (raw_text.rfind("##", 0) == 0) { if (!pending_doc.empty()) pending_doc += '\n'; pending_doc += trim(raw_text.substr(2)); continue; }
		if (scan.lines[i].continuation) {
			// Part of the statement above it: same gutter marker, but no scope,
			// declaration or diagnostic of its own.
			if ((flags & ANALYZE_SAFE_LINES) != 0 && !scan.lines[i].code.empty()) {
				model.safe_lines.push_back(line_no);
			}
			continue;
		}
		const std::string &text = statements[i];
		if (text.empty()) continue;
		while (!scopes.empty() && indent <= scopes.back().first) {
			model.declarations[size_t(scopes.back().second)].lexical_scope.end_line = line_no - 1;
			scopes.pop_back();
		}
		if ((flags & ANALYZE_SAFE_LINES) != 0) model.safe_lines.push_back(line_no);
		if ((flags & ANALYZE_DIAGNOSTICS) != 0) {
			int underflow = scan.lines[i].closing_underflow;
			for (size_t j = i + 1; j < lines.size() && scan.lines[j].continuation; j++) underflow += scan.lines[j].closing_underflow;
			if (underflow > 0) diagnostic(model, "UNEXPECTED_DELIMITER", "Unexpected closing delimiter", line_no, 1);
			if (text.back() == '=' || text.back() == ',' || text.back() == '.') diagnostic(model, "EXPECTED_EXPRESSION", "Expected expression after operator", line_no, uint32_t(text.size()));
			if (word_start(text, "func") && text.find(':') == std::string::npos) diagnostic(model, "EXPECTED_COLON", "Expected ':' after function declaration", line_no, uint32_t(text.size()));
			if ((word_start(text, "var") || word_start(text, "const")) && text.back() == ':') diagnostic(model, "EXPECTED_TYPE", "Expected a type after ':'", line_no, uint32_t(text.size()));
		}
		if ((flags & ANALYZE_DECLARATIONS) == 0) continue;
		SourceDeclaration d; bool found = false; size_t keyword = 0;
		if (word_start(text, "static func")) { d.kind = DeclarationKind::FUNCTION; keyword = 11; d.flags |= 1u; found = true; }
		else if (word_start(text, "func")) { d.kind = DeclarationKind::FUNCTION; keyword = 4; found = true; }
		else if (word_start(text, "var")) { d.kind = DeclarationKind::VARIABLE; keyword = 3; found = true; }
		else if (word_start(text, "const")) { d.kind = DeclarationKind::CONSTANT; keyword = 5; found = true; }
		else if (word_start(text, "signal")) { d.kind = DeclarationKind::SIGNAL; keyword = 6; found = true; }
		else if (word_start(text, "class_name")) { d.kind = DeclarationKind::CLASS; keyword = 10; found = true; }
		else if (word_start(text, "class")) { d.kind = DeclarationKind::NESTED_CLASS; keyword = 5; found = true; }
		else if (word_start(text, "enum")) { d.kind = DeclarationKind::ENUM; keyword = 4; found = true; }
		else if (!text.empty() && text[0] == '@') { d.kind = DeclarationKind::ANNOTATION; keyword = 1; found = true; }
		if (!found) { pending_doc.clear(); continue; }
		d.name = identifier(text, keyword); if (d.name.empty()) { pending_doc.clear(); continue; }
		const size_t name_at = text.find(d.name, keyword); d.declaration = {line_no, uint32_t(name_at + 1), line_no, uint32_t(name_at + d.name.size() + 1)};
		d.lexical_scope = {line_no, 1, uint32_t(lines.size()), uint32_t(lines.empty() ? 1 : lines.back().size() + 1)};
		d.documentation = pending_doc; pending_doc.clear(); d.parent = scopes.empty() ? -1 : scopes.back().second;
		const size_t colon = text.find(':', name_at + d.name.size());
		const size_t equals = text.find('=', name_at + d.name.size());
		if ((d.kind == DeclarationKind::VARIABLE || d.kind == DeclarationKind::CONSTANT) && colon != std::string::npos && (equals == std::string::npos || colon < equals)) d.declared_type = trim(text.substr(colon + 1, (equals == std::string::npos ? text.size() : equals) - colon - 1));
		if (d.kind == DeclarationKind::FUNCTION || d.kind == DeclarationKind::SIGNAL) {
			const size_t open = text.find('(', name_at); const size_t close = text.rfind(')');
			if (open != std::string::npos && close != std::string::npos && close > open) {
				std::stringstream params(text.substr(open + 1, close - open - 1)); std::string param;
				while (std::getline(params, param, ',')) { param = trim(param); if (param.empty()) continue; SourceParameter p; const size_t pc = param.find(':'); const size_t pe = param.find('='); p.name = trim(param.substr(0, std::min(pc, pe))); if (pc != std::string::npos) p.declared_type = trim(param.substr(pc + 1, (pe == std::string::npos ? param.size() : pe) - pc - 1)); if (pe != std::string::npos) p.default_text = trim(param.substr(pe + 1)); p.declaration = d.declaration; d.parameters.push_back(std::move(p)); }
			}
			const size_t arrow = text.find("->", close); if (arrow != std::string::npos) { const size_t end = text.find(':', arrow); d.return_type = trim(text.substr(arrow + 2, end - arrow - 2)); }
		}
		if (d.kind == DeclarationKind::ENUM) {
			const size_t open = text.find('{');
			const size_t close = text.rfind('}');
			if (open != std::string::npos && close > open) {
				std::stringstream members(text.substr(open + 1, close - open - 1));
				std::string member;
				int64_t next_value = 0;
				while (std::getline(members, member, ',')) {
					member = trim(member);
					if (member.empty()) continue;
					SourceEnumMember value;
					const size_t equal = member.find('=');
					value.name = trim(member.substr(0, equal));
					if (equal != std::string::npos) {
						try { next_value = std::stoll(trim(member.substr(equal + 1))); } catch (...) {}
					}
					value.value = next_value++;
					value.declaration = d.declaration;
					d.enum_members.push_back(std::move(value));
				}
			}
		}
		if (d.kind == DeclarationKind::ANNOTATION) {
			const size_t open = text.find('(');
			const size_t close = text.rfind(')');
			if (open != std::string::npos && close > open) {
				std::stringstream arguments(text.substr(open + 1, close - open - 1));
				std::string argument;
				while (std::getline(arguments, argument, ',')) d.annotation_arguments.push_back(trim(argument));
			}
		}
		const int32_t index = int32_t(model.declarations.size()); model.declarations.push_back(std::move(d));
		if (model.declarations.back().parent >= 0) model.declarations[size_t(model.declarations.back().parent)].children.push_back(index);
		if (model.declarations.back().kind == DeclarationKind::FUNCTION) {
			const std::vector<SourceParameter> params = model.declarations.back().parameters;
			for (const SourceParameter &parameter : params) {
				SourceDeclaration declared;
				declared.kind = DeclarationKind::PARAMETER;
				declared.name = parameter.name;
				declared.declared_type = parameter.declared_type;
				declared.resolved_type = parameter.declared_type;
				declared.declaration = parameter.declaration;
				declared.lexical_scope = model.declarations[size_t(index)].lexical_scope;
				declared.parent = index;
				const int32_t parameter_index = int32_t(model.declarations.size());
				model.declarations.push_back(std::move(declared));
				model.declarations[size_t(index)].children.push_back(parameter_index);
			}
			scopes.push_back({indent, index});
		} else if (model.declarations.back().kind == DeclarationKind::NESTED_CLASS) {
			scopes.push_back({indent, index});
		}
	}
	for (const auto &scope : scopes) {
		model.declarations[size_t(scope.second)].lexical_scope.end_line = uint32_t(lines.size());
	}
	for (SourceDeclaration &declaration : model.declarations) {
		if (declaration.parent >= 0 &&
				(declaration.kind == DeclarationKind::VARIABLE || declaration.kind == DeclarationKind::CONSTANT ||
				 declaration.kind == DeclarationKind::PARAMETER)) {
			declaration.lexical_scope = model.declarations[size_t(declaration.parent)].lexical_scope;
		}
	}
	if ((flags & ANALYZE_DIAGNOSTICS) != 0 && (flags & ANALYZE_DECLARATIONS) != 0) {
		std::unordered_map<std::string, std::string> declared_types;
		std::unordered_map<std::string, std::string> function_returns;
		for (const SourceDeclaration &declaration : model.declarations) {
			if (declaration.kind == DeclarationKind::VARIABLE ||
					declaration.kind == DeclarationKind::PARAMETER) {
				declared_types[declaration.name] = declaration.declared_type;
			} else if (declaration.kind == DeclarationKind::FUNCTION) {
				function_returns[declaration.name] = declaration.return_type;
			}
		}
		for (const SourceDeclaration &declaration : model.declarations) {
			const size_t occurrences = word_occurrences(source, declaration.name);
			if (declaration.kind == DeclarationKind::PARAMETER && occurrences <= 1) {
				warning(model, "UNUSED_PARAMETER", "Parameter '" + declaration.name + "' is never used",
						declaration.declaration.start_line, declaration.declaration.start_column);
			} else if (declaration.kind == DeclarationKind::VARIABLE && declaration.parent >= 0 && occurrences <= 1) {
				warning(model, "UNUSED_VARIABLE", "Variable '" + declaration.name + "' is never used",
						declaration.declaration.start_line, declaration.declaration.start_column);
			} else if (declaration.kind == DeclarationKind::CONSTANT && declaration.parent >= 0 && occurrences <= 1) {
				warning(model, "UNUSED_LOCAL_CONSTANT", "Local constant '" + declaration.name + "' is never used",
						declaration.declaration.start_line, declaration.declaration.start_column);
			} else if (declaration.kind == DeclarationKind::SIGNAL && occurrences <= 1) {
				warning(model, "UNUSED_SIGNAL", "Signal '" + declaration.name + "' is never used",
						declaration.declaration.start_line, declaration.declaration.start_column);
			}
			if ((declaration.kind == DeclarationKind::VARIABLE || declaration.kind == DeclarationKind::PARAMETER) && declaration.parent >= 0) {
				for (const SourceDeclaration &other : model.declarations) {
					if (&other == &declaration || other.name != declaration.name ||
							other.declaration.start_line >= declaration.declaration.start_line) continue;
					if (other.parent == -1 || other.parent == declaration.parent) {
						warning(model, "SHADOWED_VARIABLE", "'" + declaration.name + "' shadows an earlier declaration",
								declaration.declaration.start_line, declaration.declaration.start_column);
						break;
					}
				}
			}
		}
		for (size_t i = 0; i < lines.size(); i++) {
			const std::string &text = statements[i];
			if (text.empty()) continue;
			size_t next = i + 1;
			while (next < lines.size() && statements[next].empty()) next++;
			if (next < lines.size() && word_start(text, "return") &&
					indentation(lines[next]) == indentation(lines[i])) {
				warning(model, "UNREACHABLE_CODE", "Statement is unreachable", uint32_t(next + 1), 1);
			}
			if (text.find("await await") != std::string::npos) {
				warning(model, "REDUNDANT_AWAIT", "Redundant await", uint32_t(i + 1), 1);
			}
			if (text.find("assert(true") != std::string::npos || text.find("assert(false") != std::string::npos) {
				warning(model, "CONSTANT_ASSERT", "Assertion condition is constant", uint32_t(i + 1), 1);
			}
			if (word_start(text, "var") && text.find('=') == std::string::npos && text.find(':') == std::string::npos) {
				warning(model, "UNASSIGNED_VARIABLE", "Variable is declared without an initial value", uint32_t(i + 1), 1);
			}
			if (text.find("/ ") != std::string::npos || text.find(" / ") != std::string::npos) {
				const size_t slash = text.find('/');
				if (slash != std::string::npos && slash > 0 && slash + 1 < text.size() &&
						std::isdigit(static_cast<unsigned char>(text[slash - 1])) &&
						std::isdigit(static_cast<unsigned char>(text[slash + 1]))) {
					warning(model, "INTEGER_DIVISION", "Integer division discards the fractional part", uint32_t(i + 1), uint32_t(slash + 1));
				}
			}
			if (text.rfind("await ", 0) == 0 && text.find('(') == std::string::npos) {
				warning(model, "REDUNDANT_AWAIT", "Await has no asynchronous expression", uint32_t(i + 1), 1);
			}
			// Declarations and every other statement keyword: `return n`, `pass`,
			// `break` and friends are statements, not discarded expressions.
			const bool statement = statement_keyword(text);
			if (!statement && text.find('=') == std::string::npos && text.back() != ':' &&
					text.find('(') == std::string::npos &&
					(std::isalpha(static_cast<unsigned char>(text[0])) || std::isdigit(static_cast<unsigned char>(text[0])))) {
				warning(model, "STANDALONE_EXPRESSION", "Standalone expression has no effect", uint32_t(i + 1), 1);
			}

			// A source-declared non-void function used as a statement discards its
			// result. Restrict this warning to signatures we know, avoiding guesses
			// about engine or dynamically dispatched methods.
			if (!statement && text.back() == ')' && text.find('=') == std::string::npos &&
					text.rfind("await ", 0) != 0) {
				const size_t open = text.find('(');
				if (open != std::string::npos) {
					const std::string callee = trim(text.substr(0, open));
					const auto returns = function_returns.find(callee);
					if (returns != function_returns.end() && !returns->second.empty() &&
							returns->second != "void") {
						warning(model, "DISCARDED_RETURN_VALUE", "Return value of '" + callee + "' is discarded",
								uint32_t(i + 1), 1);
					}
				}
			}

			// Typed int destinations cannot retain a fractional value.
			const size_t equal = text.find('=');
			if (equal != std::string::npos) {
				std::string lhs = trim(text.substr(0, equal));
				if (word_start(lhs, "var")) lhs = trim(lhs.substr(3));
				const size_t colon = lhs.find(':');
				std::string name = trim(lhs.substr(0, colon));
				std::string type = colon == std::string::npos ? declared_types[name] : trim(lhs.substr(colon + 1));
				const std::string rhs = trim(text.substr(equal + 1));
				bool fractional = rhs.find('/') != std::string::npos;
				for (size_t at = 1; !fractional && at + 1 < rhs.size(); at++) {
					fractional = rhs[at] == '.' && std::isdigit(static_cast<unsigned char>(rhs[at - 1])) &&
							std::isdigit(static_cast<unsigned char>(rhs[at + 1]));
				}
				if (type == "int" && fractional) {
					warning(model, "NARROWING_CONVERSION", "Assignment may narrow a fractional value to int",
							uint32_t(i + 1), uint32_t(equal + 2));
				}
			}

			// Access through a source-declared but untyped value is intentionally
			// dynamic; surface the same unsafe-access warnings as GDScript.
			for (const auto &[name, type] : declared_types) {
				if (!type.empty()) continue;
				const std::string needle = name + ".";
				const size_t dot = text.find(needle);
				if (dot == std::string::npos) continue;
				const size_t member_start = dot + needle.size();
				const size_t call = text.find('(', member_start);
				const size_t boundary = text.find_first_of(" \t=,+-*/", member_start);
				if (call != std::string::npos && (boundary == std::string::npos || call < boundary)) {
					warning(model, "UNSAFE_METHOD_ACCESS", "Method access on untyped value '" + name + "' is unsafe",
							uint32_t(i + 1), uint32_t(dot + 1));
				} else {
					warning(model, "UNSAFE_PROPERTY_ACCESS", "Property access on untyped value '" + name + "' is unsafe",
							uint32_t(i + 1), uint32_t(dot + 1));
				}
				break;
			}
		}
	}
	// Apply warning annotations as lexical state after warnings have been
	// collected. Codes are compared case-insensitively with GDScript's setting
	// names (for example UNUSED_VARIABLE vs "unused_variable").
	if ((flags & ANALYZE_DIAGNOSTICS) != 0 && !model.diagnostics.empty()) {
		std::vector<std::unordered_set<std::string>> ignored(lines.size() + 1);
		std::unordered_set<std::string> active;
		std::unordered_set<std::string> one_shot;
		for (size_t i = 0; i < lines.size(); i++) {
			const std::string text = trim(lines[i]);
			auto annotation_code = [&]() {
				const size_t quote = text.find_first_of("\"'");
				if (quote == std::string::npos) return std::string();
				const size_t end = text.find(text[quote], quote + 1);
				if (end == std::string::npos) return std::string();
				std::string code = text.substr(quote + 1, end - quote - 1);
				std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return char(std::toupper(c)); });
				return code;
			};
			if (text.rfind("@warning_ignore_start", 0) == 0) active.insert(annotation_code());
			else if (text.rfind("@warning_ignore_restore", 0) == 0) active.erase(annotation_code());
			else if (text.rfind("@warning_ignore", 0) == 0) one_shot.insert(annotation_code());
			ignored[i + 1] = active;
			if (!one_shot.empty() && !text.empty() && text[0] != '@' && text[0] != '#') {
				ignored[i + 1].insert(one_shot.begin(), one_shot.end());
				one_shot.clear();
			}
		}
		model.diagnostics.erase(std::remove_if(model.diagnostics.begin(), model.diagnostics.end(),
				[&](const SourceDiagnostic &diagnostic) {
					return diagnostic.severity == DiagnosticSeverity::WARNING &&
							diagnostic.range.start_line < ignored.size() &&
							ignored[diagnostic.range.start_line].count(diagnostic.code) != 0;
				}), model.diagnostics.end());
	}
	if ((flags & ANALYZE_DIAGNOSTICS) != 0 && model.diagnostics.size() >= MAX_DIAGNOSTICS) {
		model.diagnostics.resize(MAX_DIAGNOSTICS - 1);
		model.diagnostics.push_back({DiagnosticSeverity::ERROR, "TOO_MANY_ERRORS", "Too many errors",
				model.path, {uint32_t(lines.size()), 1, uint32_t(lines.size()), 2}});
	}
	if ((flags & ANALYZE_CARET) != 0 && caret_line > 0 && size_t(caret_line) <= lines.size()) {
		const std::string before = lines[size_t(caret_line - 1)].substr(0, std::min<size_t>(size_t(std::max(caret_column, 0)), lines[size_t(caret_line - 1)].size()));
		const size_t dot = before.rfind('.');
		const size_t open = before.rfind('(');
		if (open != std::string::npos && before.find(')', open) == std::string::npos) {
			model.caret.kind = CaretKind::CALL_ARGUMENT;
			size_t end = open;
			while (end > 0 && std::isspace(static_cast<unsigned char>(before[end - 1]))) end--;
			size_t begin = end;
			while (begin > 0 && (std::isalnum(static_cast<unsigned char>(before[begin - 1])) || before[begin - 1] == '_')) begin--;
			model.caret.callee = before.substr(begin, end - begin);
			model.caret.argument_index = int32_t(std::count(before.begin() + open + 1, before.end(), ','));
		} else if (dot != std::string::npos) {
			model.caret.kind = CaretKind::MEMBER;
			size_t end = dot, begin = end;
			while (begin > 0 && (std::isalnum(static_cast<unsigned char>(before[begin - 1])) || before[begin - 1] == '_')) begin--;
			model.caret.receiver_text = before.substr(begin, end - begin);
		} else if (before.find('@') != std::string::npos) model.caret.kind = CaretKind::ANNOTATION;
		else if (before.find("->") != std::string::npos || before.rfind(':') != std::string::npos ||
				before.rfind(" is ") != std::string::npos || before.rfind(" as ") != std::string::npos) model.caret.kind = CaretKind::TYPE;
		else model.caret.kind = CaretKind::IDENTIFIER;
	}
	return model;
}

} // namespace gdscript
