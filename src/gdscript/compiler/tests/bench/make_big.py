#!/usr/bin/env python3
import re
import sys

COPIES = 60
NAMES = [
	"OP_HALT", "OP_LOADI", "OP_MOV", "OP_ADD", "OP_SUB", "OP_MUL", "OP_AND",
	"OP_OR", "OP_XOR", "OP_SHL", "OP_SHR", "OP_LT", "OP_JMP", "OP_JZ",
	"OP_JNZ", "OP_OUT",
	"mem", "reg", "out", "pc", "halted",
	"encode", "reset", "trace", "is_halted", "registers", "step",
	"step_until_halted", "run", "sum_to_program", "sum_to", "sum_to_stepped",
	"alu_trace", "unknown_opcode", "out_of_fuel",
]

def main():
	source = open(sys.argv[1]).read()
	source = "\n".join(l for l in source.split("\n") if not l.lstrip().startswith("#"))
	pattern = re.compile(r"\b(" + "|".join(sorted(NAMES, key=len, reverse=True)) + r")\b")
	chunks = []
	for i in range(COPIES):
		chunks.append(pattern.sub(lambda m: m.group(1) + "_%d" % i, source))
	sys.stdout.write("\n".join(chunks))

main()
