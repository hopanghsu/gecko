
var counter = 0;
var x = { toString: function() { counter++; } };
var y = {};

for (var i = 0; i < 50; i++)
  ++y[x];

assertEq(counter, 50);
