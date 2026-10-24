# Security Policy

ANANAS SLICER is a desktop application. It talks to exactly one kind of network
service, the Moonraker instance you configure yourself, and to nothing else: there
is no account system, no cloud service, no update check and no telemetry.

## Supported versions

ANANAS SLICER is alpha software. Only the latest release on the
[releases page](https://github.com/Extrutex/ananas-slicer/releases) receives fixes.
Older releases are not patched; please update before you report.

## Reporting a vulnerability

Please do not open a public issue for a security problem.

Report it privately through GitHub:
<https://github.com/Extrutex/ananas-slicer/security/advisories/new>

Please include the version (`Help` → `About ANANAS SLICER`), your operating system,
what the problem is, how to reproduce it, and what an attacker could do with it. A
project file, G-code file or `printer.cfg` that triggers the problem is the most
useful thing you can attach.

What you can expect:

- An acknowledgement within seven days. ANANAS SLICER has a single maintainer,
  so please allow for holidays.
- An assessment and, if the problem is confirmed, a fix in the next release. You
  are credited in the release notes if you want to be.
- Coordinated disclosure: please give the fix a chance to ship before you publish
  details. Ninety days after the report is a reasonable upper bound.

## What counts

- **Files are untrusted input.** Crashes, hangs or memory errors while opening a
  3MF, STL, OBJ, AMF, STEP, PNG or G-code file are security relevant. Please
  report them as such, with the file.
- **Moonraker connections.** ANANAS SLICER sends your API key only to the host you
  configured, over the scheme you configured. Protecting the Moonraker instance
  itself (TLS, authentication, not exposing it to the internet) is described in
  Moonraker's documentation; that is not an ANANAS SLICER issue, but we are glad
  to document safe setups in the wiki.
- **Inherited code.** ANANAS SLICER is a fork of OrcaSlicer, which descends from
  Bambu Studio, PrusaSlicer and Slic3r, and it links the libraries in `deps/`.
  Report problems in that code to us as well; we fix or forward them.
