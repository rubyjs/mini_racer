var fs = require('fs');

var start = new Date();
var babel = require ('./helper_files/babel.js');
console.log("Running V8 version " + process.versions.v8);
console.log(new Date() - start + " requiring babel");

for (var i=0; i < 10; i++) {
  start = new Date();

  babel.transform(fs.readFileSync('./helper_files/composer.js.es6', 'utf8'), {ast: false,
      whitelist: ['es6.constants', 'es6.properties.shorthand', 'es6.arrowFunctions',
                  'es6.blockScoping', 'es6.destructuring', 'es6.spread', 'es6.parameters',
                  'es6.templateLiterals', 'es6.regex.unicode', 'es7.decorators', 'es6.classes']})['code']

  console.log(new Date() - start + " transpile")
}



for (var i=0; i < 10; i++) {
  start = new Date();

  for (var j=0; j < 10; j++) {
    babel.transform(fs.readFileSync('./helper_files/composer.js.es6', 'utf8'), {ast: false,
      whitelist: ['es6.constants', 'es6.properties.shorthand', 'es6.arrowFunctions',
                  'es6.blockScoping', 'es6.destructuring', 'es6.spread', 'es6.parameters',
                  'es6.templateLiterals', 'es6.regex.unicode', 'es7.decorators', 'es6.classes']})['code']
  }

  console.log(new Date() - start + " transpile 10 times")
}
