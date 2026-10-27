CXX ?= c++
BIN_DIR ?= bin
LIB_DIR ?= lib
BUILD_DIR ?= build

PREFIX_MAP = -ffile-prefix-map=$(CURDIR)=.
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra -Wpedantic -fPIC $(PREFIX_MAP)
ASAN_CXXFLAGS ?= -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(PREFIX_MAP)
INCLUDES = -Inative/cpp/include

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    RPATH_FLAG = -Wl,-rpath,@loader_path/../$(LIB_DIR)
    SHLIB_FLAGS = -dynamiclib -Wl,-install_name,@rpath/librhea_native.so
else
    RPATH_FLAG = -Wl,-rpath,'$$ORIGIN/../$(LIB_DIR)'
    SHLIB_FLAGS = -shared
endif

.PHONY: all clean test test-asan validate-icons

all: $(LIB_DIR)/librhea_native.so $(BIN_DIR)/rhea $(BIN_DIR)/rhea_cpp_tests

$(LIB_DIR)/librhea_native.so: native/cpp/src/kernels.cpp native/cpp/include/rhea/rhea_c_api.h
	@mkdir -p $(LIB_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SHLIB_FLAGS) native/cpp/src/kernels.cpp -o $(LIB_DIR)/librhea_native.so -lm

$(BIN_DIR)/rhea: native/cpp/src/rhea_driver.cpp $(LIB_DIR)/librhea_native.so
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) native/cpp/src/rhea_driver.cpp -L$(LIB_DIR) -lrhea_native $(RPATH_FLAG) -o $(BIN_DIR)/rhea -lm

$(BIN_DIR)/rhea_cpp_tests: native/cpp/tests/test_kernels.cpp $(LIB_DIR)/librhea_native.so
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) native/cpp/tests/test_kernels.cpp -L$(LIB_DIR) -lrhea_native $(RPATH_FLAG) -o $(BIN_DIR)/rhea_cpp_tests -lm

test: all
	./$(BIN_DIR)/rhea_cpp_tests
	python3 tests/run_all_tests.py

test-asan: $(LIB_DIR)/librhea_native.so
	@mkdir -p $(BIN_DIR)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDES) native/cpp/src/kernels.cpp native/cpp/tests/test_kernels.cpp -o $(BIN_DIR)/rhea_cpp_tests_asan -lm
	./$(BIN_DIR)/rhea_cpp_tests_asan
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDES) native/cpp/src/rhea_driver.cpp -L$(LIB_DIR) -lrhea_native $(RPATH_FLAG) -o $(BIN_DIR)/rhea_asan -lm
	./$(BIN_DIR)/rhea_asan version

validate-icons:
	python3 tools/validate_icons.py

clean:
	rm -rf $(BIN_DIR)/* $(LIB_DIR)/* $(BUILD_DIR)/*
