# Directories
PROTO_DIR := ./
INCLUDE_DIR := ../../include

# Nanopb
NANOPB_DIR := ../../ext/nanopb
NANOPB_GEN := $(NANOPB_DIR)/generator/nanopb_generator.py

# Protocol Buffers
PROTO_SRC   := $(wildcard $(PROTO_DIR)/*.proto)
PROTO_PB    := $(PROTO_SRC:.proto=.pb)

default: protobuf

%.pb: %.proto
	protoc -I$(PROTO_DIR) --go_out=$(PROTO_DIR) $<
	protoc -I$(PROTO_DIR) -o$*.pb $<
	@$(NANOPB_GEN) -I$(PROTO_DIR) $@
	# pbjs -t static-module -p$(PROTO_DIR) $*.proto > $@.js
	# @mkdir -p $(SOURCE_DIR)/proto
	# @mv $*.pb.c $(SOURCE_DIR)/proto
	@mv $*.pb.h $(INCLUDE_DIR)/proto
	# protoc -I$(PROTO_DIR) --cpp_out=$(PROTO_DIR) $<

.PHONY: install
install:
	@echo moving CMakeLists.txt into place
	@mv CMake/CMakeLists.txt $(NANOPB_DIR)

.PHONY: protobuf
protobuf: protoclean $(PROTO_PB)
	@echo building the protocol buffers $(PROTO_PB)

.PHONY: protoclean
protoclean:
	@rm -fr $(PROTO_DIR)/*.pb