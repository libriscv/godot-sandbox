FROM ubuntu:24.04
#
# To build the image:
# docker build -t riscv64-linux-gnu .
#
# To run the container:
# docker run -it -v .:/usr/src riscv64-linux-gnu
#
# Once inside run 'build.sh' to build the project
# ./build.sh
#
# Install dependencies
RUN apt-get update && apt-get install -y \
	g++-13-riscv64-linux-gnu \
	ninja-build \
	cmake


# Enter the shared directory
WORKDIR /usr/src
ENTRYPOINT ["/bin/bash"]
