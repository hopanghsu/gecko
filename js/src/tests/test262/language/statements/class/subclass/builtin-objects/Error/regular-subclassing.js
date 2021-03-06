// Copyright (C) 2016 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
es6id: 19.5.1
description: Subclassing the Error object
info: >
  19.5.1 The Error Constructor

  ...
  The Error constructor is designed to be subclassable. It may be used as the
  alue of an extends clause of a class definition. Subclass constructors that
  intend to inherit the specified Error behaviour must include a super call to
  the Error constructor to create and initialize subclass instances with a
  [[ErrorData]] internal slot.
---*/

class CustomError extends Error {}

var err = new CustomError('foo 42');

assert.sameValue(err.message, 'foo 42');
assert.sameValue(err.name, 'Error');

reportCompare(0, 0);
