let
  pkgs = import (fetchTarball
    ("https://github.com/goromal/anixpkgs/archive/refs/tags/v6.8.4.tar.gz"))
    { };
in with pkgs;
mkShell {
  nativeBuildInputs = [ cpp-helper cmake ];
  buildInputs = [
    boost
    mscpp
    aapis-cpp
  ];
  shellHook = ''
    cpp-helper vscode
  '';
}
