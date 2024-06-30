CXX=g++
CXXFLAGS=-Wall -pipe -std=c++17
GLSLC=glslc
GLSLFLAGS=
LDFLAGS=$(shell pkg-config --libs vulkan) $(shell pkg-config --libs glfw3)
SRC=$(wildcard *.cpp)
OBJ=$(SRC:.cpp=.o)
SHADERSRC=$(wildcard *.glsl)
SHADEROBJ=$(SHADERSRC:.glsl=.spv)
OUT=VulkanTest

.PHONY: all debug release run clean

all: debug

debug:   CXXFLAGS += -ggdb
release: CXXFLAGS += -O2 -DNDEBUG

debug release: $(OUT) $(SHADEROBJ)

$(OUT): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LDFLAGS) -o $@

%.vert.spv: %.vert.glsl
	$(GLSLC) $(GLSLFLAGS) -fshader-stage=vert $< -o $@

%.frag.spv: %.frag.glsl
	$(GLSLC) $(GLSLFLAGS) -fshader-stage=frag $< -o $@

run: all
	./$(OUT)

clean:
	rm -f $(OBJ) $(SHADEROBJ) $(OUT)
