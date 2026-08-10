#
# Pebble SDK wscript
#

import json
import os.path

top = '.'
out = 'build'

# The settings page is bundled into the JS app rather than hosted on a web
# server: index.js serves it as a data URI, which needs the markup as a string.
# The webpack config the SDK generates only resolves .js and .json, so the page
# is wrapped in a generated module the companion can require().
CONFIG_PAGE_HTML = 'src/pkjs/config.html'
CONFIG_PAGE_JS = 'src/pkjs/config_page.js'


def generate_config_page_module(ctx):
    html = ctx.path.find_node(CONFIG_PAGE_HTML).read(encoding='utf-8')
    # ensure_ascii escapes U+2028/U+2029, which are illegal raw in a JS string
    # literal, and keeps the page's umlauts independent of the bundler's encoding.
    module = ('// Generated from %s at build time. Do not edit.\n'
              'module.exports = %s;\n'
              % (os.path.basename(CONFIG_PAGE_HTML),
                 json.dumps(html, ensure_ascii=True)))

    target = ctx.path.make_node(CONFIG_PAGE_JS)
    # Rewriting on every build would invalidate webpack's cache each time.
    if os.path.exists(target.abspath()) and target.read(encoding='utf-8') == module:
        return
    target.write(module, encoding='utf-8')


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    ctx.load('pebble_sdk')


def build(ctx):
    ctx.load('pebble_sdk')

    # Must run before the glob below so the generated module is picked up.
    generate_config_page_module(ctx)

    binaries = []

    cached_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_program(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf)
        binaries.append({'platform': platform, 'app_elf': app_elf})
        
    ctx.env = cached_env

    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries,
                   js=ctx.path.ant_glob(['src/pkjs/**/*.js', 'src/pkjs/**/*.json']),
                   js_entry_file='src/pkjs/index.js')
