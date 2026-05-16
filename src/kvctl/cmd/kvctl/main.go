// kvctl — operator CLI for the KV Cache cluster. LLD §8.3.
package main

import (
	"fmt"
	"os"
)

func main() {
	// TODO(stephen): wire cobra subcommands:
	//   inspect | tier-stats | quota | trace | members | drain
	fmt.Fprintln(os.Stderr, "kvctl: not yet implemented")
	os.Exit(1)
}
