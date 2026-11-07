name si-first-committer-wins
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

permutation waits-then-loses
1.begin
2.begin
1.del
2.del
1.end

expect waits-then-loses
2.del blocked
1.end unblocks 2.del
2.del abort
