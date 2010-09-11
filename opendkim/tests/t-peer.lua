-- $Id: t-peer.lua,v 1.2 2010/09/11 12:51:03 grooverdan Exp $

-- Copyright (c) 2010, The OpenDKIM Project.  All rights reserved.

-- PeerList calculation test

mt.echo("*** PeerList calculation test")

-- setup
sock = "unix:" .. mt.getcwd() .. "/t-peer.sock"
binpath = mt.getcwd() .. "/.."
if os.getenv("srcdir") ~= nil then
	mt.chdir(os.getenv("srcdir"))
end

-- try to start the filter
mt.startfilter(binpath .. "/opendkim", "-x", "t-peer.conf", "-p", sock)

-- try to connect to it
conn = mt.connect(sock, 40, 0.05)
if conn == nil then
	error "mt.connect() failed"
end

-- Those in the peer list should have SMFIR_ACCEPT as the result
-- to prevent any verification or signing practices

test = {
	{"localhost", "127.0.0.1", SMFIR_CONTINUE }
	, {"localhost", "192.168.1.1", SMFIR_ACCEPT }
	, {"localhost", "192.168.1.64", SMFIR_CONTINUE }
	, {"localhost", "192.168.1.128", SMFIR_CONTINUE }
	, {"localhost", "192.168.1.129", SMFIR_CONTINUE }
	, {"localhost", "192.168.1.130", SMFIR_CONTINUE }
	, {"localhost", "192.168.1.131", SMFIR_CONTINUE }
	, {"localhost", "192.168.1.132", SMFIR_ACCEPT }
	, {"localhost", "9001:db8:0:0:8:800:200c:417a", SMFIR_CONTINUE }
	, {"localhost", "2001:db8:0:0:91", SMFIR_CONTINE }
	, {"localhost", "2001:db8:0:0:128", SMFIR_CONTINUE }
	, {"localhost", "2001:db8:0:0:129", SMFIR_CONTINUE }
	, {"localhost", "2001:db8:0:0:130", SMFIR_CONTINUE }
	, {"localhost", "2001:db8:0:0:131", SMFIR_CONTINUE }
	, {"localhost", "2001:db8:0:0:132", SMFIR_ACCEPT }
	}
for index=1,table.getn(test)
do
	if mt.conninfo(conn, test[index][1], test[index][2]) ~= nil then
		error "mt.conninfo() failed"
	end
	if mt.getreply(conn) ~= test[index][3] then
		--stre = "mt.conninfo() unexpected reply " .. test[index][1] .. "(" .. test[index][2] .. ") should be " .. test[index][3]
		error "mt.conninfo() unexpected reply"
	end
end
