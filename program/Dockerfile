FROM ubuntu:24.04
#
# To build the image:
# docker build -t riscv64-linux-gnu .
#
# To run the container:
# docker run -it -v .:/usr/src riscv64-linux-gnu <my.elf>
#
# Install dependencies
RUN apt-get update && apt-get install -y \
	g++-13-riscv64-linux-gnu \
	ninja-build \
	cmake


# Enter the shared directory
WORKDIR /usr/src
# Copy API files
RUN mkdir -p /usr/api
COPY build.sh syscalls.h variant.cpp variant.hpp /usr/api/
# Set the entrypoint to the build script
ENTRYPOINT "/usr/api/build.sh" "$0" "$@"
