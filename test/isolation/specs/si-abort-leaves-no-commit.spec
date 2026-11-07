name si-abort-leaves-no-commit
isolation si

setup
INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

session 1
begin: \begin
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \abort

session 2
begin: \begin
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \commit
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }

permutation second-writer-goes-through
1.begin
2.begin
1.del
2.del
1.end
2.end
2.read

expect second-writer-goes-through
2.del blocked
1.end unblocks 2.del
2.del equals deleted 1 triple
2.read absent <http://ex/a>
