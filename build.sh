rm -rf generated/*
mkdir /tmp/IopLocNet
echo Downloading Protobuf protocol definitions
wget --quiet --output-document /tmp/IopLocNet/IopLocNet.proto3 \
    https://raw.githubusercontent.com/Internet-of-People/message-protocol/master/IopLocNet.proto3
echo Generating C++ sources from protocol definitions
protoc -I=/tmp/IopLocNet --cpp_out=generated /tmp/IopLocNet/IopLocNet.proto3
rm -rf /tmp/IopLocNet

echo Generating makefiles
rm -rf build
mkdir build
cd build
cmake ..
echo Compiling all sources
make
echo Running tests
test/tests
cd ..