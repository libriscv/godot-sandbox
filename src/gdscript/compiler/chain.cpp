#include "chain.h"
#include "compiler_exception.h"
#include <unordered_map>
#include <unordered_set>

namespace gdscript {

namespace {

[[noreturn]] void chain_error(const std::string& message, const std::string& file,
	int line, int column, const std::string& hint = "")
{
	throw CompilerException(ErrorType::SEMANTIC_ERROR, message, line, column, "", file, "", hint);
}

struct Declared {
	int link = 0;
	std::string what;
};

std::string link_label(const std::vector<ChainLink>& links, int link) {
	const ChainLink& entry = links[size_t(link)];
	if (!entry.name.empty()) {
		return "'" + entry.name + "'";
	}
	if (!entry.path.empty()) {
		return "'" + entry.path + "'";
	}
	return "this script";
}

// '@' keeps the symbol out of the method list; link number disambiguates.
std::string displaced_symbol(int link, const std::string& name) {
	return "@super" + std::to_string(link) + "." + name;
}

void reject_redeclaration(std::unordered_map<std::string, Declared>& seen,
	const std::vector<ChainLink>& links, const std::string& what, const std::string& name,
	int link, int line, int column)
{
	const auto it = seen.find(name);
	if (it != seen.end()) {
		chain_error(what + " '" + name + "' is already declared in " +
			link_label(links, it->second.link) + " as a " + it->second.what,
			links[size_t(link)].path, line, column,
			"A merged chain has one flat name table, so a base and a derived script "
			"cannot both declare '" + name + "'");
	}
	seen[name] = Declared{ link, what };
}

} // namespace

Program merge_chain(std::vector<ChainLink> links) {
	if (links.empty()) {
		return Program{};
	}
	if (links.size() == 1) {
		return std::move(links.front().program);
	}
	if (links.size() > MAX_CHAIN_DEPTH) {
		chain_error("The 'extends' chain is " + std::to_string(links.size()) +
			" scripts deep, and at most " + std::to_string(MAX_CHAIN_DEPTH) + " are merged",
			links.back().path, links.back().program.base_class_line,
			links.back().program.base_class_column,
			"Every base in the chain is compiled into this program");
	}

	{
		std::unordered_set<std::string> seen_paths;
		for (const ChainLink& link : links) {
			if (link.path.empty()) {
				continue;
			}
			if (!seen_paths.insert(link.path).second) {
				chain_error("'" + link.path + "' appears twice in the 'extends' chain",
					link.path, 0, 0, "A script cannot extend itself, directly or through a base");
			}
		}
	}

	const int leaf = int(links.size()) - 1;
	Program merged;
	Program& script = links[size_t(leaf)].program;

	for (const ChainLink& link : links) {
		merged.is_tool = merged.is_tool || link.program.is_tool;
	}

	merged.class_name = script.class_name;
	merged.class_name_line = script.class_name_line;
	merged.class_name_column = script.class_name_column;
	merged.base_class = script.base_class;
	merged.base_is_path = script.base_is_path;
	merged.base_class_line = script.base_class_line;
	merged.base_class_column = script.base_class_column;

	merged.native_base_class = links.front().program.base_class;
	merged.native_base_is_path = links.front().program.base_is_path;

	for (const ChainLink& link : links) {
		merged.chain.class_names.push_back(link.program.class_name.empty()
			? link.name : link.program.class_name);
		merged.chain.paths.push_back(link.path);
	}

	std::unordered_map<std::string, Declared> members;
	std::unordered_map<std::string, Declared> types;

	for (int link = 0; link <= leaf; link++) {
		Program& source = links[size_t(link)].program;

		for (VarDeclStmt& global : source.globals) {
			reject_redeclaration(members, links, global.is_const ? "Constant" : "Variable",
				global.name, link, global.line, global.column);
			global.chain_link = link;
			for (FunctionDecl* accessor : { global.setter_body.get(), global.getter_body.get() }) {
				if (accessor != nullptr) {
					accessor->chain_link = link;
				}
			}
			merged.globals.push_back(std::move(global));
		}
		for (SignalDecl& signal : source.signals) {
			reject_redeclaration(members, links, "Signal", signal.name, link,
				signal.line, signal.column);
			merged.signals.push_back(std::move(signal));
		}
		for (StructDecl& decl : source.structs) {
			reject_redeclaration(types, links, decl.is_class ? "Class" : "Struct", decl.name,
				link, decl.line, decl.column);
			for (FunctionDecl& method : decl.methods) {
				method.chain_link = link;
			}
			merged.structs.push_back(std::move(decl));
		}
		for (EnumDecl& decl : source.enums) {
			if (!decl.name.empty()) {
				reject_redeclaration(types, links, "Enum", decl.name, link, decl.line, decl.column);
			}
			merged.enums.push_back(std::move(decl));
		}
	}

	std::unordered_map<std::string, size_t> visible;
	for (int link = 0; link <= leaf; link++) {
		Program& source = links[size_t(link)].program;
		std::unordered_set<std::string> declared_here;

		for (FunctionDecl& decl : source.functions) {
			const std::string name = decl.name;
			if (!declared_here.insert(name).second) {
				chain_error("Function '" + name + "' is declared twice in " +
					link_label(links, link), links[size_t(link)].path, decl.line, decl.column);
			}
			decl.chain_link = link;

			std::vector<ChainInfo::Origin>& origins = merged.chain.functions[name];
			if (!origins.empty()) {
				ChainInfo::Origin& displaced = origins.back();
				displaced.symbol = displaced_symbol(displaced.link, name);
				FunctionDecl& previous = merged.functions[visible.at(name)];
				previous.chain_name = name;
				previous.name = displaced.symbol;
			}
			origins.push_back(ChainInfo::Origin{ link, name });

			visible[name] = merged.functions.size();
			merged.functions.push_back(std::move(decl));
		}
	}

	return merged;
}

} // namespace gdscript
