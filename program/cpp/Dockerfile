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
	git cmake mold ccache

# Enter the shared directory
WORKDIR /usr/src
# Copy API files
RUN mkdir -p /usr/api
COPY build.sh api/* /usr/api/

# Set the entrypoint to the build script
#ENTRYPOINT "/usr/api/build.sh" "$0" "$@"
CMD ["/bin/bash", "-c", "tail -f /dev/null"]
