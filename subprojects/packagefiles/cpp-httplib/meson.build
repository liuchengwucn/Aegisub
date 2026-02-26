project('cpp-httplib', 'cpp',
  version: '0.33.1',
  license: 'MIT',
)

# Pure header-only dependency with no OpenSSL/SSL support.
# MCP server only needs plain HTTP on localhost.
cpp_httplib_dep = declare_dependency(
  include_directories: include_directories('.'),
)

meson.override_dependency('cpp-httplib', cpp_httplib_dep)
