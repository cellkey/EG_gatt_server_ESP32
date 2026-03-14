# Push This Project to Your GitHub Repository

Step-by-step guide to put your ESP32 project on GitHub.

---

## Prerequisites

- **Git** installed on your PC ([git-scm.com](https://git-scm.com/))
- **GitHub account** ([github.com](https://github.com/))
- Project folder open in terminal (e.g. `EG_gatt_server_ESP32` or your project root)

---

## 1. Create a new repository on GitHub

1. Log in to [GitHub](https://github.com/).
2. Click **"+"** (top right) → **"New repository"**.
3. Fill in:
   - **Repository name:** e.g. `EG_gatt_server_ESP32` (or any name you like).
   - **Description:** optional (e.g. "ESP32 BLE GATT server with relay control").
   - **Public** or **Private** — your choice.
   - **Do not** check "Add a README", "Add .gitignore", or "Choose a license" if you already have a project locally (to avoid merge issues).
4. Click **"Create repository"**.
5. Copy the repository URL (e.g. `https://github.com/YOUR_USERNAME/EG_gatt_server_ESP32.git` or the SSH form if you use SSH).

---

## 2. Open terminal in your project folder

- **Windows:** In File Explorer, go to your project folder, then in the address bar type `cmd` or `powershell` and press Enter.  
  Or in VS Code/Cursor: **Terminal → New Terminal** (it usually opens in the workspace root).
- **macOS/Linux:** `cd` to your project directory.

Make sure you are in the folder that contains your `main/`, `CMakeLists.txt`, and (if present) `.gitignore`.

---

## 3. Initialize Git (if not already)

Check if Git is already set up:

```bash
git status
```

- If you see something like "not a git repository": initialize and do the first commit (steps 3a–3c).
- If you see a list of files or "nothing to commit, working tree clean": skip to **Section 4**.

**3a. Initialize repository**

```bash
git init
```

**3b. Add all files**

Your project should have a `.gitignore` (e.g. ignoring `build/`, `sdkconfig.old`, IDE files). Then:

```bash
git add .
```

**3c. First commit**

```bash
git commit -m "Initial commit: ESP32 BLE GATT server project"
```

---

## 4. Connect to your GitHub repository

Replace `YOUR_USERNAME` and `REPO_NAME` with your GitHub username and repository name:

```bash
git remote add origin https://github.com/cellkey/REPO_NAME.git
```

Example:

```bash
git remote add origin https://github.com/celllkey/EG_gatt_server_ESP32.git
```

If you prefer **SSH** (after [setting up SSH keys](https://docs.github.com/en/authentication/connecting-to-github-with-ssh)):

```bash
git remote add origin git@github.com:YOUR_USERNAME/REPO_NAME.git
```

**If "origin" already exists** and you want to change it:

```bash
git remote set-url origin https://github.com/YOUR_USERNAME/REPO_NAME.git
```

---

## 5. Push to GitHub

**If this is the first push** (and you created the repo empty):

```bash
git branch -M main
git push -u origin main
```

- `git branch -M main` renames your current branch to `main` (GitHub’s default).
- `git push -u origin main` uploads your commits and sets `origin main` as the default for future `git push` / `git pull`.

**If GitHub suggested "push an existing repository"** and you already have commits:

```bash
git push -u origin main
```

When prompted for credentials:

- **HTTPS:** use your GitHub username and a **Personal Access Token** (not your account password). Create one: GitHub → Settings → Developer settings → Personal access tokens.
- **SSH:** no password if your key is set up.

---

## 6. Later: make changes and push again

After editing code:

```bash
git add .
git commit -m "Short description of what you changed"
git push
```

---

## Quick reference

| Step              | Command |
|-------------------|--------|
| Check status      | `git status` |
| Add all files     | `git add .` |
| Commit            | `git commit -m "Your message"` |
| Add remote (once)  | `git remote add origin https://github.com/cellkey/EG_gatt_server_ESP32.git
| First push        | `git branch -M main` then `git push -u origin main` |
| Later pushes      | `git push` |
  add new branch    | git push --set-upstream origin branch_140326
---

## Notes

- **`.gitignore`** should already exclude `build/`, `sdkconfig.old`, and IDE/OS junk so they are not pushed. If something big or secret is being committed, add it to `.gitignore` and run `git add .` and `git commit` again.
- **Large files:** GitHub has limits (e.g. 100 MB per file). Keep build outputs and binaries out of the repo via `.gitignore`.
- **Two-factor authentication:** With 2FA, use a Personal Access Token over HTTPS instead of your account password.
