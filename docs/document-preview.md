# Documentation preview

Open a `.md`, `.markdown`, `.mdown`, `.mkd`, `.adoc`, `.asciidoc`, `.asc`, `.rst` or `.rest` file and use the **Preview** and **Split** controls at the right of the tab strip. The View menu has the same preview commands; `Ctrl+Shift+V` toggles the rendered view. Split mode keeps the source editable beside the preview and scrolling either pane moves the other pane. The **GitHub** / **GitLab** control switches the preview's link-color profile.

The native renderer is intentionally self-contained and does not execute HTML, scripts, or remote content. It supports headings, paragraphs, emphasis, inline code, links, lists, quotations, rules, Markdown tables and fenced code blocks; AsciiDoc source blocks and lists; and reStructuredText underlined headings, lists and `code-block` / `sourcecode` directives. Recognized code fences use Hootedit's lexer for syntax colors. Images are shown as descriptive placeholders rather than fetched or decoded. The preview reports missing local references and raw HTML that it displays as text.

These are GitHub/GitLab-inspired previews, not pixel-identical copies of either hosted renderer. Extensions, plugins, reference-style syntax, embedded media, raw HTML, and platform-specific features can change the rendered result on a forge; use the notes as checks, not as a guarantee of remote equivalence.
