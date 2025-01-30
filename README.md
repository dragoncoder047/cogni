# Cogni

This is an experimental interpreter for the [Cognate](https://github.com/cognate-lang/cognate) programming language that parses and runs the code directly and doesn't have to compile to C first.

In its present state it runs fairly well, if slowly. As of 37022ba89fea08482035ebc5ea498a81f54fa094 most of the functions are implemented but a few (5 out of 31) tests still error out because the functions they need aren't implemented. The `maths` and `box` tests run and print "FAIL" results, but this is currently kind of intentional (see #4 for `maths` and cognate-lang/cognate#40 for `box`).
