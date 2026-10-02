name upgrade-deadlock
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
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \abort

permutation cycle
1.begin
2.begin
1.read
2.read
1.del
2.del

expect cycle
1.del blocked
2.del abort
2.del unblocks 1.del
