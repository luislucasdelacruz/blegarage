{
  pkgs,
  lib,
  config,
  inputs,
  ...
}:

{
  packages = [
    pkgs.git
    pkgs.platformio
    pkgs.just
    pkgs.caddy
    pkgs.openssl
  ];

  enterShell = ''
    mkdir -p .devenv/state/tls
        if [ ! -f .devenv/state/tls/cert.pem ]; then
          lan_ip=$(hostname -I 2>/dev/null | awk '{print $1}')
          sans="DNS:localhost,IP:127.0.0.1"
          [ -n "$lan_ip" ] && sans="$sans,IP:$lan_ip"
          if openssl req -x509 -newkey rsa:2048 -nodes \
              -keyout .devenv/state/tls/key.pem -out .devenv/state/tls/cert.pem \
              -days 3650 -subj "/CN=localhost" \
              -addext "subjectAltName=$sans" >/dev/null 2>&1; then
            chmod 600 .devenv/state/tls/key.pem
          fi
        fi
  '';
}
