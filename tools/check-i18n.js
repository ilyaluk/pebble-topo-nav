#!/usr/bin/env node
// Verifies the settings page translations: every data-i18n key and every t.<key>
// lookup must exist in all dictionaries, and no dictionary entry may be unused.
// Run: node tools/check-i18n.js

var fs = require('fs');
var path = require('path');

var file = path.join(__dirname, '..', 'src', 'pkjs', 'config.html');
var html = fs.readFileSync(file, 'utf8');

var dictBlock = html.match(/var translations = \{[\s\S]*?\n  \};/);
if (!dictBlock) {
  console.error('check-i18n: could not locate the translations object');
  process.exit(1);
}

var dicts = {};
var langRe = /^    ([a-z]{2}): \{$/gm;
var match;
while ((match = langRe.exec(dictBlock[0])) !== null) {
  var rest = dictBlock[0].slice(match.index);
  var body = rest.slice(0, rest.indexOf('\n    }'));
  var keys = [];
  var keyRe = /^      ([a-zA-Z0-9_]+):/gm;
  var k;
  while ((k = keyRe.exec(body)) !== null) keys.push(k[1]);
  dicts[match[1]] = keys;
}

var langs = Object.keys(dicts);
if (langs.length < 2) {
  console.error('check-i18n: expected at least two dictionaries, found ' + langs.join(', '));
  process.exit(1);
}

function collect(re, source) {
  var out = [], m;
  while ((m = re.exec(source)) !== null) out.push(m[1]);
  return out;
}

var attrKeys = collect(/data-i18n(?:-placeholder)?="([a-zA-Z0-9_]+)"/g, html);
var codeKeys = collect(/(?:\bt|getTranslations\(\))\.([a-zA-Z0-9_]+)/g,
                       html.slice(dictBlock.index + dictBlock[0].length));
var used = {};
attrKeys.concat(codeKeys).forEach(function(key) { used[key] = true; });

var errors = [];

Object.keys(used).forEach(function(key) {
  langs.forEach(function(lang) {
    if (dicts[lang].indexOf(key) === -1) {
      errors.push('missing from "' + lang + '" dictionary: ' + key);
    }
  });
});

langs.forEach(function(lang) {
  dicts[lang].forEach(function(key) {
    if (!used[key]) errors.push('unused in "' + lang + '" dictionary: ' + key);
  });
});

var ids = collect(/getElementById\("([a-zA-Z0-9-]+)"\)/g, html);
ids.forEach(function(id) {
  if (html.indexOf('id="' + id + '"') === -1) errors.push('getElementById on missing id: ' + id);
});

if (errors.length) {
  errors.forEach(function(e) { console.error('check-i18n: ' + e); });
  process.exit(1);
}

console.log('check-i18n: ' + Object.keys(used).length + ' keys OK across ' + langs.join(', ') +
            ' (' + attrKeys.length + ' in markup, ' + ids.length + ' element lookups)');
