# Test certificates

**For tests only. The private key here is public and protects nothing.**

- `spectera-mock.crt`, `spectera-mock.key`: a self-signed RSA-2048 certificate for
  `spectera-mock.invalid`, valid for a hundred years, made with

  ```
  openssl req -x509 -newkey rsa:2048 -nodes -keyout spectera-mock.key \
      -out spectera-mock.crt -days 36500 -subj "/CN=spectera-mock.invalid"
  ```

  `tests/blackbox/mock_spectera.py` serves HTTPS with it, as a Spectera base station
  serves its own self-signed one, so `blackbox.radios` drives Go.dot's TLS stream, its
  certificate pin and its refusal of a certificate it does not trust (namespace draft
  §124, BET). Its SHA-256 fingerprint is
  `DE:9B:8D:71:97:F6:33:68:30:55:83:DC:C6:72:84:05:17:6F:C7:9B:3F:A0:39:AC:7D:E9:C9:DB:D1:CF:C7:3D`.
