PROJECT="/sdcard/Documents/new-game-project"

## For --debug argument:
if [ "$1" == "--debug" ]; then
	echo "Building in debug mode..."
	scons platform=android target=template_debug debug_symbols=yes ndk_version=24.0.8215888
	#adb push bin/addons/godot_sandbox/bin/libgodot_riscv.android.template_release.arm64.so $PROJECT/addons/godot_sandbox/bin/libgodot_riscv.android.template_release.arm64.so
	cp bin/addons/godot_sandbox/bin/libgodot_riscv.android.template_debug.arm64.so bin/addons/godot_sandbox/bin/libgodot_riscv.android.template_release.arm64.so
	adb push bin/addons/godot_sandbox/bin/libgodot_riscv.android.template_debug.arm64.so $PROJECT/addons/godot_sandbox/bin/libgodot_riscv.android.template_release.arm64.so
## For --backtrace:
elif [ "$1" == "--backtrace" ]; then
	adb logcat | /usr/lib/android-ndk/ndk-stack -sym bin/addons/godot_sandbox/bin
else
## Default case: build in release mode
	echo "Building in release mode..."
	scons platform=android target=template_release ndk_version=24.0.8215888
	adb push bin/addons/godot_sandbox/bin/libgodot_riscv.android.template_release.arm64.so $PROJECT/addons/godot_sandbox/bin/libgodot_riscv.android.template_release.arm64.so
fi
