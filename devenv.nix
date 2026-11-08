{ pkgs, lib, config, inputs, ... }:

{
  packages = [
    pkgs.git
    pkgs.platformio
    pkgs.just
  ];
}
