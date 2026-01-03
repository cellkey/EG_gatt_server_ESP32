# Git Commands Reference

A quick reference guide for common Git commands.

## Basic Commands

### Repository Status
```bash
git status                    # Check the status of your working directory
git log                       # View commit history
git log --oneline            # View compact commit history
```

### Staging and Committing
```bash
git add <file>               # Stage a specific file
git add .                    # Stage all changes
git add -A                   # Stage all changes including deletions
git commit -m "message"      # Commit staged changes with a message
git commit -am "message"     # Stage and commit all modified files (skips untracked)
```

### Viewing Changes
```bash
git diff                     # View unstaged changes
git diff --staged            # View staged changes
git diff HEAD                # View all changes (staged + unstaged)
git show <commit-hash>       # View details of a specific commit
```

## Branch Management

### Working with Branches
```bash
git branch                   # List all local branches
git branch <name>            # Create a new branch
git checkout <branch>        # Switch to a branch
git checkout -b <branch>     # Create and switch to a new branch
git branch -d <branch>       # Delete a branch (safe delete)
git branch -D <branch>       # Force delete a branch
```

### Merging
```bash
git merge <branch>           # Merge a branch into current branch
git merge --no-ff <branch>  # Create a merge commit even if fast-forward is possible
```

## Remote Repository (GitHub)

### Setting Up Remote
```bash
git remote add origin <url>  # Add a remote repository
git remote -v                # View remote repositories
git remote remove origin     # Remove a remote repository
```

### Pushing and Pulling
```bash
git push -u origin main      # Push to remote and set upstream (first time)
git push                     # Push changes to remote
git pull                     # Pull changes from remote
git fetch                    # Fetch changes without merging
git fetch origin             # Fetch from specific remote
```

### Cloning
```bash
git clone <url>              # Clone a repository
git clone <url> <folder>     # Clone into a specific folder
```

## Undoing Changes

### Unstaging and Discarding
```bash
git restore <file>           # Discard changes in working directory (unstaged)
git restore --staged <file>  # Unstage a file (keeps changes)
git reset HEAD <file>        # Unstage a file (alternative)
git checkout -- <file>       # Discard changes (older syntax)
```

### Amending Commits
```bash
git commit --amend           # Amend the last commit message
git commit --amend --no-edit # Amend last commit without changing message
```

### Resetting
```bash
git reset --soft HEAD~1      # Undo last commit, keep changes staged
git reset --mixed HEAD~1     # Undo last commit, keep changes unstaged
git reset --hard HEAD~1      # Undo last commit, discard all changes (DANGEROUS)
```

## Configuration

### User Configuration
```bash
git config --global user.name "Your Name"
git config --global user.email "your.email@example.com"
git config --list            # View all configuration
git config user.name         # View specific setting
```

### Local vs Global
```bash
git config --global ...      # Set for all repositories
git config ...               # Set for current repository only
```

## Useful Tips

### Ignoring Files
- Edit `.gitignore` file to exclude files from tracking
- Patterns: `*.log`, `build/`, `node_modules/`, etc.

### Viewing History
```bash
git log --graph --oneline --all  # Visual branch history
git log --since="2 weeks ago"    # Filter by time
git log --author="name"           # Filter by author
```

### Stashing (Temporary Save)
```bash
git stash                     # Save changes temporarily
git stash list                # List stashes
git stash pop                 # Apply and remove most recent stash
git stash apply               # Apply stash but keep it
git stash drop                # Delete a stash
```

## Common Workflows

### Daily Workflow
```bash
git status                    # Check what changed
git add .                     # Stage changes
git commit -m "Description"   # Commit changes
git push                      # Push to remote (if connected)
```

### Starting New Feature
```bash
git checkout -b feature-name # Create and switch to new branch
# ... make changes ...
git add .
git commit -m "Add feature"
git push -u origin feature-name
```

### Updating from Remote
```bash
git pull                      # Fetch and merge from remote
# Or:
git fetch                     # Fetch changes
git merge origin/main         # Merge fetched changes
```

## Getting Help
```bash
git help <command>            # Get help for a specific command
git <command> --help          # Alternative help syntax
```

