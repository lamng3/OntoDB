name lost-update
isolation rc

setup
INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

session 1
begin: \begin
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \commit

session 2
begin: \begin
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
end: \commit

permutation once
1.begin
2.begin
1.del
2.del
1.end
2.read
2.end

expect once
2.del blocked
1.end unblocks 2.del
2.read absent <http://ex/a>
