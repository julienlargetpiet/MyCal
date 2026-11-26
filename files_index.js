"use strict";

document.addEventListener("DOMContentLoaded", () => {
    const createForm = document.getElementById("create-directory-form");
    const directoryInput = document.getElementById("directory-name");
    const deleteButton = document.getElementById("delete-selected");
    const message = document.getElementById("files-message");

    // /static/julien/images/ -> images
    const components = window.location.pathname
        .split("/")
        .filter(Boolean);

    const currentDirectory = components
        .slice(2)
        .map(decodeURIComponent)
        .join("/");

    async function request(endpoint, header, path) {
        const response = await fetch(endpoint, {
            method: "POST",
            credentials: "same-origin",
            headers: {
                "X-MyCal-Request": "1",
                [header]: path
            }
        });

        const body = await response.text();

        if (!response.ok)
            throw new Error(body || `HTTP ${response.status}`);

        window.location.reload();
    }

    function reportError(error) {
        message.textContent = error.message;
    }

    document.querySelectorAll(
        "input[name='selected-entry']"
    ).forEach(input => {
        input.addEventListener("change", () => {
            deleteButton.disabled = false;
        });
    });

    createForm.addEventListener("submit", async event => {
        event.preventDefault();

        const name = directoryInput.value.trim();

        if (!/^[A-Za-z0-9_-]+$/.test(name)) {
            message.textContent = "Invalid folder name";
            return;
        }

        const currentPath = decodeURIComponent(window.location.pathname)
            .replace(/^\/|\/$/g, "");
        
        const path = `${currentPath}/${name}`;

        message.textContent = "Creating folder...";

        try {
            await request(
                "/directory/create",
                "X-Directory",
                path
            );
        } catch (error) {
            reportError(error);
        }
    });

    deleteButton.addEventListener("click", async () => {
        const selected = document.querySelector(
            "input[name='selected-entry']:checked"
        );

        if (!selected)
            return;

        const name = selected.value;
        const directory = selected.dataset.kind === "directory";

        const currentPath = decodeURIComponent(window.location.pathname)
            .replace(/^\/|\/$/g, "");
        
        const path = `${currentPath}/${name}`;

        const type = directory ? "folder" : "file";

        if (!window.confirm(`Delete ${type} "${name}"?`))
            return;

        deleteButton.disabled = true;
        message.textContent = `Deleting ${type}...`;

        try {
            await request(
                directory ? "/directory/delete" : "/file/delete",
                directory ? "X-Directory" : "X-File",
                path
            );
        } catch (error) {
            reportError(error);
            deleteButton.disabled = false;
        }
    });
});



