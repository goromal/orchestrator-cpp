let
  pkgs = import (fetchTarball
    ("https://github.com/goromal/anixpkgs/archive/9d703eccfd7421964a5fe90106efb358eebf70d0.tar.gz"))
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
