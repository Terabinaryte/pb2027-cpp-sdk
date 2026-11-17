# Vendored dependencies

| Directory | Upstream | Version / commit | License |
| --- | --- | --- | --- |
| `websocketclient/` | https://github.com/rbeeli/websocketclient-cpp | commit `96a3f3f` (CHANGELOG 0.7.3) | [MIT](websocketclient/LICENSE) |
| `doctest/` | https://github.com/doctest/doctest | v2.4.11 | [MIT](doctest/LICENSE.txt) |
| `httplib/` | https://github.com/yhirose/cpp-httplib | v0.66.0 | [MIT](httplib/LICENSE) |
| `nlohmann/` | https://github.com/nlohmann/json | v3.11.3 | [MIT](nlohmann/LICENSE.MIT) |

Vendored so that `cmake --build` works offline. Each directory keeps the
upstream license text next to the vendored files. Do not edit these files;
update by replacing the directory, its license file, and recording the new
version here.
