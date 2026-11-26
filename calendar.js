const calendar = document.querySelector("#calendar");

if (calendar) {
    const year = Number(calendar.dataset.year);
    const month = Number(calendar.dataset.month);

    // Generate month heading
    const monthName = new Intl.DateTimeFormat("en", {
        month: "long",
    }).format(new Date(year, month - 1, 1));

    const header = document.createElement("header");
    header.className = "calendar-header";

    const eyebrow = document.createElement("p");
    eyebrow.className = "calendar-eyebrow";
    eyebrow.textContent = "MYCAL / PERSONAL CALENDAR";

    const title = document.createElement("h1");
    title.className = "calendar-title";
    title.textContent = `${monthName} ${year}`;

    //header.append(eyebrow, title);
    //calendar.before(header);

    const toggle = document.querySelector("#theme-toggle");
    
    const titleRow = document.createElement("div");
    titleRow.className = "calendar-title-row";
    
    titleRow.append(title, toggle);
    
    header.append(eyebrow, titleRow);
    calendar.before(header);


    // Highlight today
    const today = new Date();
    const isCurrentMonth =
        today.getFullYear() === year &&
        today.getMonth() + 1 === month;

    // Event dialog
    const dialog = document.querySelector("#event-dialog");
    const dialogTitle = dialog.querySelector(".event-dialog-title");
    const dialogDescription = dialog.querySelector(".event-dialog-description");

    // Close dialog with the close button
    dialog.querySelector(".event-dialog-close")
        .addEventListener("click", () => dialog.close());

    // Close dialog when clicking outside its visible area
    dialog.addEventListener("click", event => {
        if (event.target !== dialog)
            return;

        const rect = dialog.getBoundingClientRect();

        if (event.clientX < rect.left ||
            event.clientX > rect.right ||
            event.clientY < rect.top ||
            event.clientY > rect.bottom) {
            dialog.close();
        }
    });

    const cells = calendar.querySelectorAll("td");

    for (const cell of cells) {
        const dayElement = cell.querySelector(".day-number");

        if (!dayElement)
            continue;

        const day = Number(
            dayElement.textContent.replace(/^D/, "")
        );

        if (!Number.isInteger(day) || day < 1 || day > 31)
            continue;

        const weekday = new Intl.DateTimeFormat("en", {
            weekday: "short"
        }).format(new Date(year, month - 1, day));
        
        const weekdayElement = document.createElement("span");
        weekdayElement.className = "day-weekday";
        weekdayElement.textContent = weekday;
        
        cell.querySelector(".day-container").append(weekdayElement);

        // Saturday and Sunday
        if (cell.cellIndex >= 5)
            cell.classList.add("weekend");

        // Current day
        if (isCurrentMonth && day === today.getDate())
            cell.classList.add("today");

        // Select day on click, except when opening event details
        cell.addEventListener("click", event => {
            if (event.target.closest(".event-details-button"))
                return;

            calendar.querySelector(".selected")
                ?.classList.remove("selected");

            cell.classList.add("selected");
        });
    }

    // Open event dialog using a single delegated listener
    calendar.addEventListener("click", event => {
        const button = event.target.closest(".event-details-button");

        if (!button)
            return;

        const cell = button.closest("td");
        const day = cell.querySelector(".day-number").textContent;
        const description = cell.querySelector(".day-events");

        dialogTitle.textContent = `${monthName} ${day}, ${year}`;

        // Copy the event description, preserving <br> elements
        dialogDescription.replaceChildren(
            ...Array.from(
                description.childNodes,
                node => node.cloneNode(true)
            )
        );

        dialog.showModal();
    });

    // Event editor
    const editDialog = document.querySelector("#event-edit-dialog");
    const editForm = editDialog.querySelector(".event-edit-form");
    const editInput = editDialog.querySelector("#event-input");
    const editStatus = editDialog.querySelector(".event-edit-status");
    const cancelEdit = editDialog.querySelector(".event-cancel");
    const saveEdit = editDialog.querySelector(".event-save");
    
    let editingCell = null;
    
    function getEvents(cell) {
        return cell.querySelector(".day-events").dataset.rawDescription;
    }    

    // Open the editor.
    calendar.addEventListener("click", event => {
        const button = event.target.closest(".event-edit-button");
        if (!button) return;
    
        editingCell = button.closest("td");
    
        const day = editingCell.querySelector(".day-number").textContent;
    
        editDialog.querySelector("h2").textContent =
            `Edit ${monthName} ${day}, ${year}`;
    
        editInput.value = getEvents(editingCell);
        editStatus.textContent = "";
    
        editDialog.showModal();
    });
    
    cancelEdit.addEventListener("click", () => editDialog.close());
    
    // Save changes.
    editForm.addEventListener("submit", async event => {
        event.preventDefault();
    
        if (!editingCell) return;
    
        const day = Number(
            editingCell.querySelector(".day-number").textContent
        );
    
        const events = editInput.value.replace(/\r\n?/g, "\n")
            .split("\n").join("+");
   
        const body = `${year},${month},${day},${events}`;
    
        if (events.includes("\0") ||
            new TextEncoder().encode(body).length > 12000) {
            editStatus.textContent = "Invalid or excessively long event description.";
            return;
        }
    
        saveEdit.disabled = true;
        cancelEdit.disabled = true;
        editStatus.textContent = "Saving...";
    
        try {
            const response = await fetch("/events", {
                method: "POST",
                headers: {
                    "Content-Type": "text/plain",
                    "X-MyCal-Request": "1"
                },
                body
            });
    
            if (!response.ok)
                throw new Error(`HTTP ${response.status}`);
    
            window.location.reload();
    
        } catch (error) {
            editStatus.textContent = `Unable to save: ${error.message}`;
        } finally {
            saveEdit.disabled = false;
            cancelEdit.disabled = false;
        }
    });

    // Navigate between days using arrow keys.
    document.addEventListener("keydown", event => {
        const offsets = {
            ArrowLeft: -1,
            ArrowRight: 1,
            ArrowUp: -7,
            ArrowDown: 7
        };
    
        const offset = offsets[event.key];
    
        if (offset === undefined)
            return;
    
        // Don't interfere with text editing or dialogs.
        if (event.target.closest("textarea") ||
            document.querySelector("dialog[open]"))
            return;
    
        const days = Array.from(
            calendar.querySelectorAll("td:has(.day-number)")
        );
    
        const selected = calendar.querySelector("td.selected");
    
        // Start at today's date if the current month is displayed,
        // otherwise start at the first day.
        const index = selected
            ? days.indexOf(selected)
            : isCurrentMonth ? today.getDate() - 1 : 0;
    
        const next = index + offset;
    
        if (next < 0 || next >= days.length)
            return;
    
        event.preventDefault();
    
        selected?.classList.remove("selected");
        days[next].classList.add("selected");
    });

    // Navigation buttons generated by calendar.c
    const previous = document.querySelector("#previous-month");
    const next = document.querySelector("#next-month");
    
    // Disable previous navigation before January 1970
    previous.disabled = year === 1970 && month === 1;
    
    async function navigate(offset) {
        const date = new Date(year, month - 1 + offset, 1);
    
        const targetYear = date.getFullYear();
        const targetMonth = date.getMonth() + 1;
    
        if (targetYear < 1970)
            return;
    
        previous.disabled = true;
        next.disabled = true;
    
        try {
            const response = await fetch("/navigate", {
                method: "POST",
                headers: {
                    "Content-Type": "text/plain",
                    "X-MyCal-Request": "1"
                },
                body: `${targetYear},${targetMonth}`
            });
    
            if (!response.ok)
                throw new Error(`HTTP ${response.status}`);
    
            const url = await response.text();
    
            window.location.assign(url.trim());
    
        } catch (error) {
            alert(`Unable to navigate: ${error.message}`);
    
            previous.disabled = year === 1970 && month === 1;
            next.disabled = false;
        }
    }
    
    previous.addEventListener("click", () => navigate(-1));
    next.addEventListener("click", () => navigate(1));

}

// Theme toggle
const toggle = document.querySelector("#theme-toggle");
const icon = toggle.querySelector(".theme-icon");
const root = document.documentElement;

const systemDark = window.matchMedia("(prefers-color-scheme: dark)");

let savedTheme;

try {
    savedTheme = localStorage.getItem("mycal-theme");
} catch {
    savedTheme = null;
}

function setTheme(theme) {
    root.dataset.theme = theme;

    icon.textContent = theme === "dark" ? "☀️" : "🌙";

    toggle.setAttribute(
        "aria-label",
        theme === "dark"
            ? "Switch to light mode"
            : "Switch to dark mode"
    );
}

// Apply saved preference or system theme
setTheme(
    savedTheme === "dark" || savedTheme === "light"
        ? savedTheme
        : systemDark.matches ? "dark" : "light"
);

// Toggle light/dark mode
toggle.addEventListener("click", () => {
    const next = root.dataset.theme === "dark" ? "light" : "dark";

    setTheme(next);
    savedTheme = next;

    try {
        localStorage.setItem("mycal-theme", next);
    } catch {
        // Storage may be unavailable for local files.
    }
});

// Follow system theme until the user selects a preference
systemDark.addEventListener("change", event => {
    if (savedTheme !== "dark" && savedTheme !== "light") {
        setTheme(event.matches ? "dark" : "light");
    }
});

const fileInput = document.querySelector("#file-input");
const uploadButton = document.querySelector("#upload-button");
const uploadStatus = document.querySelector("#upload-status");
const uploadDirectory = document.querySelector("#upload-directory");

if (fileInput && uploadButton && uploadStatus && uploadDirectory) {
    uploadButton.addEventListener("click", async () => {
        const file = fileInput.files[0];

        if (!file) {
            uploadStatus.textContent = "Please select a file.";
            return;
        }

        if (file.size === 0 || file.size > 25 * 1024 * 1024) {
            uploadStatus.textContent = "File must be between 1 byte and 25 MiB.";
            return;
        }

        uploadButton.disabled = true;
        uploadStatus.textContent = "Uploading...";

        try {
            const response = await fetch("/upload", {
                method: "POST",
                headers: {
                    "Content-Type": "application/octet-stream",
                    "X-MyCal-Request": "1",
                    "X-Filename": encodeURIComponent(file.name),
                    "X-Upload-Directory": uploadDirectory.value
                },
                body: file
            });

            const result = await response.text();

            if (!response.ok)
                throw new Error(result);

            uploadStatus.textContent = "Upload successful!";
            window.location.reload();
        } catch (error) {
            uploadStatus.textContent = `Upload failed: ${error.message}`;
        } finally {
            uploadButton.disabled = false;
        }
    });
}






