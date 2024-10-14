let
  pkgs = import (fetchTarball
    ("https://github.com/goromal/anixpkgs/archive/refs/heads/dev/bump.tar.gz")) # TODO: shuffle
    { };
in with pkgs;
mkShell {
  nativeBuildInputs = [ cpp-helper cmake ];
  buildInputs = [
    aapis-cpp
    protobuf
    mscpp
    boost
  ];
  shellHook = ''
    cpp-helper vscode
  '';
}
