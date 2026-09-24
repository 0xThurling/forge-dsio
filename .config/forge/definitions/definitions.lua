---@meta

---
--- Forge Lua API Type Definitions
--- Auto-generated for Lua Language Server (lua_ls)
---

--- Global forge API object
---@class forge
forge = {}


--- Forge logging utility
---@class forge.log
forge.log = {}


--- Access Forge's config
---@class forge.config
forge.config = {}


--- Adds custom CMake commands to the generated CMakeLists.txt
---@param snippet string The custom CMake command
function forge.add_cmake(snippet) end


--- Logs an information message to the console
---@param message string The message to log
function forge.log.info(message) end


--- Logs a warning message to the console
---@param message string The message to log
function forge.log.warn(message) end


--- Logs a error message to the console
---@param message string The message to log
function forge.log.error(message) end


--- Clones a Git repository to external/
---@param repo_url string The URL for the GitHub repository
---@param tag string The URL for the GitHub repository
---@return string The path location where it's saved'
function forge.pull_repo(repo_url, tag) end


--- Installs packages using the Specified package manager
---@param password string Password or 'nopass'
---@param package_manager string The package manager
---@param packages string[] List of packages
---@return number 0 on success
function forge.get_packages(password, package_manager, packages) end


--- Gets a config value by key
---@param key string Dot-notation key (e.g., 'project.name')
---@return string The config value
function forge.config.get(key) end


--- Checks if a feature is enabled
---@param feature string Feature name
---@return boolean True if enabled
function forge.config.has_feature(feature) end


--- Gets a feature option value
---@param feature string Feature name
---@param option string Option name
---@param default string Default value if not set
---@return string The option value
function forge.config.get_feature_option(feature, option, default) end


--- Operating System information
---@class forge.os
forge.os = {}


--- The current operating system
---@type string
forge.os.current = ""


--- The Windows operating system
---@type string
forge.os.windows = ""


--- The MacOS operating system
---@type string
forge.os.macos = ""


--- The Linux operating system
---@type string
forge.os.linux = ""


--- Linux distribution information
---@class forge.distro
forge.distro = {}


--- Current Linux distrobution
---@type string
forge.distro.my_distro = ""


--- The Arch Linux distrobution... btw
---@type string
forge.distro.arch = ""


--- The NixOs Linux distrobution
---@type string
forge.distro.nixos = ""


--- The Debian Linux distrobution
---@type string
forge.distro.debian = ""


--- The Ubuntu Linux Distrobution
---@type string
forge.distro.ubuntu = ""


--- The Manjaro Linux distrobution
---@type string
forge.distro.manjaro = ""


--- The Fedora Linux distrobution
---@type string
forge.distro.fedora = ""


--- Unknown Linux distrobution
---@type string
forge.distro.unknown = ""


--- Package manager constants
---@class forge.package_manager
forge.package_manager = {}


--- Use no password for package manager
---@type string
forge.package_manager.no_pass = ""


--- The WinGet package manager for Windows
---@type string
forge.package_manager.winget = ""


--- The Chocolatey package manager for Windows
---@type string
forge.package_manager.chocolatey = ""


--- The Homebrew package manager for MacOs
---@type string
forge.package_manager.brew = ""


--- The Pacman package manager for Arch
---@type string
forge.package_manager.pacman = ""


--- The APT package manager for Debian/Ubuntu
---@type string
forge.package_manager.aptget = ""


--- Downloads a file from a URL
---@param url string The URL to download
---@param output string The output file path
function forge.download(url, output) end


--- Extracts an archive file
---@param archive string Path to archive file
---@param output string Output directory
---@param string_components number The number of path components to strip (default: 1)
function forge.extract(archive, output, string_components) end


--- Downloads and extracts an archive in one step
---@param url string The URL to fetch
---@return string The path to the newly fetched directory
function forge.fetch(url) end


--- The current working directory
---@type string
forge.current_working_dir = ""


--- Runs a shell command and captures its output
---@param command string The command line to run
---@return number The exit code
---@return string The combined output
function forge.exec(command) end


--- Reads a file, relative to the project root
---@param path string The file to read
---@return string The contents, or nil
function forge.read_file(path) end


--- Writes a file, creating parent directories
---@param path string The file to write
---@param contents string The contents to write
function forge.write_file(path, contents) end


--- Copies a file, creating parent directories
---@param source string The file to copy
---@param destination string Where to copy it
function forge.copy_file(source, destination) end


--- Creates a directory (and its parents)
---@param path string The directory to create
function forge.mkdir(path) end


--- Renders @KEY@ placeholders from a file into another file
---@param source string The template file
---@param destination string The file to write
---@param values table Values for the @KEY@ placeholders
function forge.template(source, destination, values) end


--- Reads the project's Git state
---@class forge.git
forge.git = {}


--- The newest tag with distance and dirty flag (git describe)
---@return string The description, or nil
function forge.git.describe() end


--- The abbreviated commit hash
---@return string The hash, or nil
function forge.git.rev() end


--- The exact tag at HEAD, or nil
---@return string The tag, or nil
function forge.git.tag() end


--- The current branch name
---@return string The branch, or nil
function forge.git.branch() end


--- Whether the working tree has uncommitted changes
---@return boolean True when dirty
function forge.git.dirty() end


--- Registers a named CMake section at a chosen anchor
---@param name string The section name
---@param position string before:<section>, after:<section>, first, last or a number
---@param content string The CMake to emit
function forge.add_section(name, position, content) end


--- Sets a config value and writes it back to forge.lua
---@param key string The config key
---@param value string The value to set
function forge.config.set(key, value) end


