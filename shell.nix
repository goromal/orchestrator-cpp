let
  pkgs = import (fetchTarball
    ("https://github.com/goromal/anixpkgs/archive/5f50d09b512c87349b3411bc90a0291838878e5e.tar.gz"))
    { };
in with pkgs;
mkShell {
  nativeBuildInputs = [ cpp-helper cmake ];
  buildInputs = [
    boost
    mscpp
    aapis-cpp
    protobuf
    spdlog
    catch2
    sqlite
  ];
}
