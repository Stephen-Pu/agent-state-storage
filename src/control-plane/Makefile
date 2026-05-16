# Go proto codegen for the Control Plane.
#
# The Go stubs land in internal/pb/. Run `make proto` whenever the .proto files
# under ../core/proto/ change.

PROTO_DIR := ../core/proto
OUT_DIR   := internal/pb
PROTOS    := events.proto node.proto cp.proto

.PHONY: proto
proto:
	mkdir -p $(OUT_DIR)
	protoc \
		-I=$(PROTO_DIR) \
		--go_out=$(OUT_DIR)      --go_opt=paths=source_relative \
		--go-grpc_out=$(OUT_DIR) --go-grpc_opt=paths=source_relative \
		$(addprefix $(PROTO_DIR)/,$(PROTOS))

.PHONY: build
build:
	go build ./...

.PHONY: test
test:
	go test ./...
