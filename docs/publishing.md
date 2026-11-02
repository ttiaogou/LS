# Publishing This Fork

The public branch contains a reviewed source snapshot on top of upstream history.
Local development history is retained separately and must not be pushed: it may
contain obsolete APKs, local configuration and internal research.

Keep the following local only:

- API keys, `local.yml`, signing stores and signing passwords.
- APK/AAB outputs, native build outputs, caches and logs.
- Extracted upstream binaries, decompiled material, internal handoff documents
  and closed-binary patch tools.

Keep source tests, Gradle wrapper files and third-party source licenses. They are
build/verification inputs, not disposable outputs. ShadowHook source provenance
is recorded in `third_party-shadowhook/UPSTREAM.md`.

Before pushing, inspect `git status`, `git diff --cached --stat` and
`git ls-tree -r HEAD`. Push only the reviewed `main` branch; do not use
`git push --all`, `--mirror` or automatic tag following from the local archive.
The fork's inherited upstream history is not a guarantee of secret-free history.
Do not advertise unverified device compatibility or resolved runtime bugs based
only on build/test success.

GitHub Actions are initially disabled to avoid automatically building/uploading
artifacts or publishing a release before signing and device acceptance are ready.
Enable them deliberately after reviewing the release workflow and its secrets.
