set -e
GODOT_VERSION=4.4.1

# Check if unit tests are run from Github Actions
if [ -n "$CI" ]; then
	GODOT=./Godot_v4.4.1-stable_linux.x86_64
	# Use the --import flag to properly initialize the project
	$GODOT --path "$PWD" --headless --import
else
	# Use a local Godot binary
	if [ -z "$GODOT" ]; then
		GODOT=~/Godot_v4.4.1-stable_linux.x86_64
	fi
fi

# Try to find the RISC-V cross-compiler
if command -v riscv64-linux-gnu-g++-14 >/dev/null 2>&1; then
	export CXX="riscv64-linux-gnu-g++-14"
elif command -v riscv64-linux-gnu-g++ >/dev/null 2>&1; then
	export CXX="riscv64-linux-gnu-g++"
else
	echo "Error: RISC-V cross-compiler not found. Please install g++-riscv64-linux-gnu"
	exit 1
fi
#export CXX="riscv64-unknown-elf-g++"

# Build the unit test ELF file
mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake -DSTRIPPED=OFF -DFLTO=ON
make -j4
popd

# Create a symbolic link to the unit test ELF file
ln -fs ../.build/unittests tests/tests.elf

# Import again for CI
if [ -n "$CI" ]; then
	$GODOT --path "$PWD" --headless --import
fi

# Run the unit tests using the GUT addon
# Use a temporary file to capture output while still showing it
TEMP_OUTPUT=$(mktemp)
trap "rm -f $TEMP_OUTPUT" EXIT

# Run tests and capture output while displaying it
$GODOT --path "$PWD" --headless -s addons/gut/gut_cmdln.gd $@ 2>&1 | tee "$TEMP_OUTPUT"
EXIT_CODE=${PIPESTATUS[0]}

# Check if GDExtension failed to load (tests can't run without it)
if grep -q "GDExtension dynamic library not found" "$TEMP_OUTPUT"; then
	echo "ERROR: GDExtension library failed to load. Tests cannot run."
	exit 1
fi

# Check if no tests were run (another indicator of failure)
if grep -q "Nothing was run" "$TEMP_OUTPUT"; then
	echo "ERROR: No tests were executed. This usually indicates the GDExtension failed to load."
	exit 1
fi

exit $EXIT_CODE
