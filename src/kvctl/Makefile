# Go proto codegen for kvctl.
#
# Phase A2.2-trace — kvctl trace dials the kvstore-node NodeData.Subscribe
# gRPC stream, so it needs Go stubs for node.proto + events.proto. They
# land in internal/nodepb/ with the go_package overridden to kvctl's
# module (the .proto files declare control-plane's package). The stubs
# are gitignored (*.pb.go); run `make proto` after cloning or whenever
# the .proto files change.
#
# Pre-req: protoc-gen-go + protoc-gen-go-grpc on $PATH
#   go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
#   go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest

PROTO_DIR := ../core/proto
OUT_DIR   := internal/nodepb
PKG       := github.com/Stephen-Pu/kvcache/kvctl/internal/nodepb

.PHONY: proto
proto:
	mkdir -p $(OUT_DIR)
	protoc \
		-I=$(PROTO_DIR) \
		--go_out=$(OUT_DIR)      --go_opt=paths=source_relative \
		--go_opt=Mevents.proto=$(PKG) --go_opt=Mnode.proto=$(PKG) \
		--go-grpc_out=$(OUT_DIR) --go-grpc_opt=paths=source_relative \
		--go-grpc_opt=Mevents.proto=$(PKG) --go-grpc_opt=Mnode.proto=$(PKG) \
		events.proto node.proto

.PHONY: build
build: proto
	go build ./...

.PHONY: test
test: proto
	go test ./...
