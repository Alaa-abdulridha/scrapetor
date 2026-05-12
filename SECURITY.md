# Security Policy

## Reporting a vulnerability

If you've found a security issue in Scrapetor, please **do not** open
a public GitHub issue. Instead, email the maintainer:

- **Alaa Abdulridha** &lt;alaa@serpapi.com&gt;

Include:

- A description of the issue and its impact
- Steps to reproduce (a minimal proof-of-concept is ideal)
- The Scrapetor version, Ruby version, and operating system
- Whether you'd like to be credited in the advisory

You should receive an acknowledgement within 72 hours. A fix and
coordinated disclosure timeline will be agreed via email.

## Supported versions

Only the latest minor release receives security fixes. Once a new
minor version is released, the prior minor branch is supported for 90
days for high-severity fixes.

## Scope

Security reports are accepted for issues in code shipped by the
`scrapetor` gem itself. Issues in upstream dependencies (development-only
gems used by tests or benchmarks) should be reported to those projects.

## Hardening notes for users

Scrapetor parses HTML received from arbitrary sources. The native
engine is fuzzed against malformed inputs (truncated tags, unclosed
quotes, mismatched closes, deep nesting up to the 1024-frame ceiling,
adversarial entity sequences). If you're parsing untrusted input at
scale, set a wall-clock timeout on the worker and constrain the
maximum document size you'll accept — the same precautions you'd apply
to any HTML parser.
