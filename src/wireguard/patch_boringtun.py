#!/usr/bin/env python3
"""Idempotent source patches to make boringtun work on this MIPS/embedded kernel.
Currently: make the IPv6 listen socket best-effort (kernel has no IPv6 -> udp6=None,
run v4-only) instead of hard-failing open_listen_socket with EAFNOSUPPORT."""
import sys, pathlib

mod = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else \
      pathlib.Path.home() / "btbuild/boringtun/boringtun/src/device/mod.rs"
src = mod.read_text()

MARKER = "// V6-OPTIONAL-PATCH"

OLD = (
'        let udp_sock6 = socket2::Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;\n'
'        udp_sock6.set_reuse_address(true)?;\n'
'        udp_sock6.bind(&SocketAddrV6::new(Ipv6Addr::UNSPECIFIED, port, 0, 0).into())?;\n'
'        udp_sock6.set_nonblocking(true)?;\n'
'\n'
'        self.register_udp_handler(udp_sock4.try_clone().unwrap())?;\n'
'        self.register_udp_handler(udp_sock6.try_clone().unwrap())?;\n'
'        self.udp4 = Some(udp_sock4);\n'
'        self.udp6 = Some(udp_sock6);\n'
)

NEW = (
'        ' + MARKER + ': IPv6 may be entirely absent (embedded kernels); tolerate failure.\n'
'        let udp_sock6 = (|| -> Result<socket2::Socket, Error> {\n'
'            let s = socket2::Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;\n'
'            s.set_reuse_address(true)?;\n'
'            s.bind(&SocketAddrV6::new(Ipv6Addr::UNSPECIFIED, port, 0, 0).into())?;\n'
'            s.set_nonblocking(true)?;\n'
'            Ok(s)\n'
'        })().ok();\n'
'\n'
'        self.register_udp_handler(udp_sock4.try_clone().unwrap())?;\n'
'        if let Some(ref s6) = udp_sock6 {\n'
'            self.register_udp_handler(s6.try_clone().unwrap())?;\n'
'        }\n'
'        self.udp4 = Some(udp_sock4);\n'
'        self.udp6 = udp_sock6;\n'
)

if MARKER in src:
    print("listen patch already present")
elif OLD not in src:
    print("ERROR: expected udp6 block not found (boringtun version drift?)", file=sys.stderr)
    sys.exit(2)
else:
    src = src.replace(OLD, NEW, 1)
    print("listen patch applied")

# Both packet handlers assume udp6 is always Some. With no IPv6 (udp6=None) the
# iface handler .expect()s and panics, and the 250ms timer handler early-returns
# (so no handshake/keepalive ever fires). Make udp6 optional in both; it is only
# ever *used* to send to a V6 endpoint, which we never have.
EDITS = [
  # timer handler: don't require udp6 in the tuple match
  ('(Some(udp4), Some(udp6)) => (udp4, udp6),',
   '(Some(udp4), udp6) => (udp4, udp6),'),
  # timer handler: guard the V6 send (udp6 is now Option)
  ('udp6.send_to(packet, &endpoint_addr.into()).ok()',
   'udp6.and_then(|u6| u6.send_to(packet, &endpoint_addr.into()).ok())'),
  # iface handler: don't panic when udp6 is absent
  ('let udp6 = d.udp6.as_ref().expect("Not connected");',
   'let udp6 = d.udp6.as_ref();'),
  # iface handler: guard the V6 send
  ('let _: Result<_, _> = udp6.send_to(packet, &addr.into());',
   'if let Some(u6) = udp6 { let _: Result<_, _> = u6.send_to(packet, &addr.into()); }'),
]
for old, new in EDITS:
    if new in src:
        continue
    if old not in src:
        print(f"ERROR: expected fragment not found: {old!r}", file=sys.stderr); sys.exit(3)
    src = src.replace(old, new, 1)

mod.write_text(src)
print("v6 patch applied (listen + timer + iface handlers)")
