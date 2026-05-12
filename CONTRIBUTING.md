# Contributing

Thanks for taking the time to look at Scrapetor. Pull requests and
issue reports are welcome.

## Filing an issue

Before opening an issue, please search existing ones to see if it has
already been reported. When opening a new issue, include:

- Ruby version (`ruby -v`)
- Scrapetor version (`gem list scrapetor`)
- Operating system
- A minimal, reproducible example
- The behaviour you expected and what happened instead

For performance regressions, please include the relevant
`benchmark/comprehensive.rb` output.

## Local development

```
git clone https://github.com/Alaa-abdulridha/scrapetor
cd scrapetor
bundle install
rake compile
rake test
```

The native extension is built with `mkmf` (`ext/scrapetor/native/extconf.rb`).
It needs a working C compiler — `clang` on macOS, `gcc` on Linux. No
other system libraries are required.

## Running the test suite

```
rake test                                   # full suite (158 tests)
ruby -Ilib -Itest test/test_scrapetor.rb    # one file
```

The benchmark suite is in `benchmark/`. Every benchmark asserts that
the three engines (Scrapetor, Nokolexbor, Nokogiri) produce equivalent
output before timing.

## Pull requests

- Branch from `main`.
- Keep changes focused. One change per pull request.
- Add tests for new behaviour and for any bug fix.
- For changes that affect performance, include benchmark numbers before
  and after.
- Don't bump the gem version in your pull request — the maintainer does
  that as part of a release.

## Coding style

- Ruby: two-space indent, single-quote strings unless interpolating,
  no trailing whitespace.
- C: same indent style as the rest of `scrapetor_native.c`.
- Public Ruby methods should have a brief comment when their behaviour
  isn't obvious from the name. Don't write docstrings just to restate
  the signature.

## Releasing

This section is for maintainers.

1. Update `lib/scrapetor/version.rb`.
2. Add a CHANGELOG entry under a new version heading.
3. `bundle install && rake test && rake bench` — both should pass clean.
4. Tag the commit (`git tag v0.x.y && git push --tags`).
5. `gem build scrapetor.gemspec && gem push scrapetor-0.x.y.gem`.

## License

By contributing, you agree that your contributions will be licensed
under the project's MIT License.
