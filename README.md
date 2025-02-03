# Cogni

This is an experimental interpreter for the [Cognate](https://github.com/cognate-lang/cognate) programming language that parses and runs the code directly and doesn't have to compile to C first.

In its present state it runs fairly well, if slowly. All of the functions from CognaC are implemented, but the `maths`, `table`, and `box` tests run and print "FAIL" results. This is currently intentional (see [#4](https://github.com/dragoncoder047/cogni/issues/4) for `maths` and cognate-lang/cognate#40 for `box` and `table`).
