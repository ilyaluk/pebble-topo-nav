#!/usr/bin/env node
// Verifies the settings page translations: every data-i18n key and every t.<key>
// lookup must exist in all dictionaries, no dictionary entry may be unused, and
// the markup text (which the page falls back to) must match the English entry.
// Run: node tools/check-i18n.js

var fs = require('fs');
var path = require('path');

var file = process.argv[2] || path.join(__dirname, '..', 'src', 'pkjs', 'config.html');
var html = fs.readFileSync(file, 'utf8');

var dictBlock = html.match(/var translations = \{[\s\S]*?\n  \};/);
if (!dictBlock) {
  console.error('check-i18n: could not locate the translations object');
  process.exit(1);
}

// lang -> { key: value }
var dicts = {};
var langRe = /^    ([a-z]{2}): \{$/gm;
var match;
while ((match = langRe.exec(dictBlock[0])) !== null) {
  var rest = dictBlock[0].slice(match.index);
  var body = rest.slice(0, rest.indexOf('\n    }'));
  var entries = {};
  var entryRe = /^      ([a-zA-Z0-9_]+): "((?:[^"\\]|\\.)*)",?$/gm;
  var entry;
  while ((entry = entryRe.exec(body)) !== null) {
    entries[entry[1]] = entry[2].replace(/\\"/g, '"').replace(/\\\\/g, '\\');
  }
  dicts[match[1]] = entries;
}

var langs = Object.keys(dicts);
if (langs.length < 2) {
  console.error('check-i18n: expected at least two dictionaries, found ' + langs.join(', '));
  process.exit(1);
}
if (langs.indexOf('en') === -1) {
  console.error('check-i18n: no "en" dictionary to compare markup fallbacks against');
  process.exit(1);
}

function collect(re, source) {
  var out = [], m;
  while ((m = re.exec(source)) !== null) out.push(m[1]);
  return out;
}

function decode(text) {
  return text.replace(/&amp;/g, '&').replace(/&lt;/g, '<')
             .replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&#39;/g, "'");
}

// Quotes and key charset stay permissive here so malformed attributes are
// reported as such instead of silently not matching.
var attrRe = /data-i18n(-placeholder)?\s*=\s*("|')([^"']*)\2/g;
var attrs = [];
var attr;
while ((attr = attrRe.exec(html)) !== null) {
  attrs.push({ key: attr[3], isPlaceholder: !!attr[1], index: attr.index, end: attrRe.lastIndex });
}
var attrKeys = attrs.map(function(a) { return a.key; });

// Scanning around the dictionary rather than only after it, so lookups written
// above it are covered too.
var codeKeys = collect(/(?:\bt|getTranslations\(\))\.([a-zA-Z0-9_]+)/g,
                       html.slice(0, dictBlock.index) +
                       html.slice(dictBlock.index + dictBlock[0].length));

var used = {};
attrKeys.concat(codeKeys).forEach(function(key) { used[key] = true; });

var errors = [];

attrKeys.forEach(function(key) {
  if (!/^[a-zA-Z0-9_]+$/.test(key)) errors.push('malformed data-i18n key: "' + key + '"');
});

Object.keys(used).forEach(function(key) {
  langs.forEach(function(lang) {
    if (!dicts[lang].hasOwnProperty(key)) {
      errors.push('missing from "' + lang + '" dictionary: ' + key);
    }
  });
});

langs.forEach(function(lang) {
  Object.keys(dicts[lang]).forEach(function(key) {
    if (!used[key]) errors.push('unused in "' + lang + '" dictionary: ' + key);
  });
});

// The markup text is what a viewer sees when a dictionary lacks the key, so it
// has to stay in step with the English entry.
attrs.forEach(function(a) {
  var expected = dicts.en[a.key];
  if (expected === undefined) return; // already reported above
  var tagEnd = html.indexOf('>', a.end);
  if (tagEnd === -1) return;
  var actual;
  if (a.isPlaceholder) {
    var tagStart = html.lastIndexOf('<', a.index);
    var ph = html.slice(tagStart, tagEnd).match(/\splaceholder\s*=\s*("|')([\s\S]*?)\1/);
    if (!ph) {
      errors.push('data-i18n-placeholder="' + a.key + '" on an element with no placeholder attribute');
      return;
    }
    actual = ph[2];
  } else {
    actual = html.slice(tagEnd + 1, html.indexOf('<', tagEnd + 1));
  }
  actual = decode(actual).trim();
  if (actual !== expected) {
    errors.push('markup text for "' + a.key + '" does not match the en entry:\n' +
                '      markup: ' + JSON.stringify(actual) + '\n' +
                '      en:     ' + JSON.stringify(expected));
  }
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
