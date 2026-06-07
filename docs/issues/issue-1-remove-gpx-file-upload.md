# Issue 1: Remove GPX File Upload Option from Settings Webview

## User Story
**As a** TopoNav user,
**I want** to input GPX route coordinates only via copy-pasting raw GPX text,
**So that** I am not confused by an unreliable file selector interface.

## Acceptance Criteria
1. **Remove File Input Element**: The file selection input (`<input type="file" ...>`) for GPX files must be removed from the settings Webview HTML (`config.html`).
2. **Remove File-Related Labels & Instructions**: Any labels, buttons, or helper texts mentioning "Choose File", "Upload GPX File", or similar upload-related instructions must be deleted.
3. **Preserve GPX Text Input Area**: The existing text area field for copying and pasting raw GPX content must remain fully visible and functional as the primary input mechanism.
4. **Verify Settings Save Flow**: Pasting a GPX string into the text area must continue to parse, simplify (using the Douglas-Peucker algorithm), and activate the route correctly upon saving.
5. **UI Refactoring & Polish**: Clean up the form layout in `config.html` to ensure that removing the file upload elements does not leave empty visual gaps, broken styles, or dead spacer elements.
