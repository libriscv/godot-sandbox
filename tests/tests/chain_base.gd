extends Node
class_name SgdChainBase

# The base half of an `extends <ScriptClass>` chain. A .sgd leaf extends this,
# and the compiler folds this body into the leaf's program: a sandboxed program
# is one binary built from one file, so nothing here is reachable at run time
# unless it was compiled in. Deliberately a plain .gd -- GDScript cannot take a
# SafeGDScript as a base, so the base of a chain is always the .gd side.

const CHAIN_CENTER := 320

enum ChainShape { RECTANGLE, CIRCLE }

var base_calls := 0

static func doubled(value):
	return value * 2

# Calls a method the leaf overrides: with one flat name table after the merge,
# this reaches the override rather than the copy declared here.
func describe():
	return "base:" + chain_kind()

func chain_kind():
	return "base"
