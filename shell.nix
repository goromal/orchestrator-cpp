let
  pkgs = import (fetchTarball
    ("https://github.com/goromal/anixpkgs/archive/refs/heads/dev/o2.tar.gz"))
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
  ];
}
