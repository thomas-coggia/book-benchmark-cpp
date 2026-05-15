# Git hooks

Enable once per clone so `git commit` rejects misformatted C/C++ (matches `.clang-format` in the repo root):

```bash
git config core.hooksPath .githooks
```

Hooks run for any client that commits normally (including automation). Avoid `git commit --no-verify` unless you intend to bypass checks.
