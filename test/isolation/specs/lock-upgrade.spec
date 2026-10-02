name lock-upgrade
isolation rc

setup
INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

session 1
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \commit

session 2
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
end: \commit

permutation upgrade
1.begin
2.begin
1.read
2.read
1.del
2.end
1.end

expect upgrade
1.del blocked
2.end unblocks 1.del
