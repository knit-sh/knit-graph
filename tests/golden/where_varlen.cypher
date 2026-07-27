MATCH (a:Foo)-[:calls*1..3]-(b) WHERE a.x > 1 AND b.name STARTS WITH 'abc' RETURN DISTINCT a
