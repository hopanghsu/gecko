// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es5id: 15.4.4.16-7-1
description: >
    Array.prototype.every considers new elements added to array after
    the call
---*/

  var calledForThree = false;

  function callbackfn(val, Idx, obj)
  {
    arr[2] = 3;
    if(val == 3)
      calledForThree = true;
    return true;
  }

  var arr = [1,2,,4,5];
  
  var res = arr.every(callbackfn);

assert(calledForThree, 'calledForThree !== true');

reportCompare(0, 0);
