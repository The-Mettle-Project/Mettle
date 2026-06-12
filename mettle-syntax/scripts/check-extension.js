const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');

function readJson(relativePath) {
  const fullPath = path.join(root, relativePath);
  try {
    return JSON.parse(fs.readFileSync(fullPath, 'utf8'));
  } catch (err) {
    throw new Error(`${relativePath}: ${err.message}`);
  }
}

function assertFile(relativePath) {
  const fullPath = path.join(root, relativePath);
  if (!fs.existsSync(fullPath)) {
    throw new Error(`Missing file: ${relativePath}`);
  }
}

function assertContributionFiles(pkg) {
  for (const language of pkg.contributes.languages || []) {
    if (language.configuration) assertFile(language.configuration);
    if (language.icon) {
      if (language.icon.light) assertFile(language.icon.light);
      if (language.icon.dark) assertFile(language.icon.dark);
    }
  }
  for (const grammar of pkg.contributes.grammars || []) {
    assertFile(grammar.path);
    readJson(grammar.path);
  }
  for (const snippet of pkg.contributes.snippets || []) {
    assertFile(snippet.path);
    readJson(snippet.path);
  }
}

function assertCommandHandlers(pkg) {
  const extensionText = fs.readFileSync(path.join(root, 'extension.js'), 'utf8');
  for (const command of pkg.contributes.commands || []) {
    const needle = `registerCommand('${command.command}'`;
    if (!extensionText.includes(needle)) {
      throw new Error(`Command is contributed but not registered: ${command.command}`);
    }
  }
}

function assertLanguageFeatures() {
  const extensionText = fs.readFileSync(path.join(root, 'extension.js'), 'utf8');
  const requiredNeedles = [
    'registerHoverProvider',
    'provideMettleHover',
    'provideImportPathHover',
    'provideDeclarationHover',
  ];
  for (const needle of requiredNeedles) {
    if (!extensionText.includes(needle)) {
      throw new Error(`Missing language feature wiring: ${needle}`);
    }
  }

  if (!extensionText.includes('registerLanguageFeatures')) {
    throw new Error('Missing language feature wiring: registerLanguageFeatures');
  }
  const languageText = fs.readFileSync(path.join(root, 'language.js'), 'utf8');
  const requiredProviders = [
    'registerDefinitionProvider',
    'registerReferenceProvider',
    'registerRenameProvider',
    'registerDocumentSymbolProvider',
    'registerWorkspaceSymbolProvider',
    'registerCompletionItemProvider',
    'registerSignatureHelpProvider',
    'registerInlayHintsProvider',
    'registerDocumentLinkProvider',
    'registerCodeLensProvider',
  ];
  for (const needle of requiredProviders) {
    if (!languageText.includes(needle)) {
      throw new Error(`Missing navigation provider wiring: ${needle}`);
    }
  }

  const requiredHoverTopics = [
    'import_str',
    'errdefer',
    'cstring',
    'static_assert',
    'where',
    'match',
    'cstr',
    'malloc',
    'memcpy',
    'fopen',
  ];
  for (const topic of requiredHoverTopics) {
    const topicPattern = new RegExp(`title:\\s*'${topic.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}($|[ '\\(])`);
    if (!topicPattern.test(extensionText)) {
      throw new Error(`Missing curated hover topic: ${topic}`);
    }
  }
}

function assertNoMojibake(relativePath) {
  const text = fs.readFileSync(path.join(root, relativePath), 'utf8');
  if (/[\u00e2\u20ac\u2122\u0153\u009d]/.test(text)) {
    throw new Error(`${relativePath}: contains mojibake characters`);
  }
}

function main() {
  const pkg = readJson('package.json');
  readJson('language-configuration.json');
  assertContributionFiles(pkg);
  assertCommandHandlers(pkg);
  assertLanguageFeatures();
  assertNoMojibake('package.json');
  assertNoMojibake('README.md');

  if (!pkg.version || !/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(pkg.version)) {
    throw new Error(`package.json: invalid semver version '${pkg.version}'`);
  }

  console.log('mettle-syntax extension check passed');
}

try {
  main();
} catch (err) {
  console.error(err.message);
  process.exit(1);
}
