name si-committed-before-write
isolation si

setup
INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

session 1
begin: \begin
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
end: \commit

session 2
begin: \begin
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

permutation already-committed
1.begin
2.begin
1.del
1.end
2.del

expect already-committed
2.del abort
