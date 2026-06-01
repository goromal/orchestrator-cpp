let
  # Use local anixpkgs for development
  pkgs = import ../anixpkgs { };
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
