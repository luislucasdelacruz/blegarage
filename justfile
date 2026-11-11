build:
	pio run

flash:
	pio run -t upload

monitor:
	pio device monitor

compiledb:
	pio run -t compiledb

serve:
	caddy run --config Caddyfile --adapter caddyfile
