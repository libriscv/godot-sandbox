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
	g++-14-riscv64-linux-gnu \
	g++ git cmake

# Clone and build mold
WORKDIR /usr/mold
RUN git clone https://github.com/rui314/mold.git /usr/mold
RUN mkdir -p /usr/mold/build && cd /usr/mold/build && cmake .. && make -j8
RUN cd /usr/mold/build && make install
RUN rm -rf /usr/mold

# Enter the shared directory
WORKDIR /usr/src
# Copy API files
RUN mkdir -p /usr/api
COPY build.sh api/* /usr/api/
# Set the entrypoint to the build script
ENTRYPOINT "/usr/api/build.sh" "$0" "$@"
