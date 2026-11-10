#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# Entry point: python -m agent_gateway --config agent-gateway.toml

import argparse
import asyncio
import logging
import os

import uvicorn

from . import config, server
from .repos import Repos
from .runner import Runner
from .store import Store


async def main() -> None:
   ap = argparse.ArgumentParser(prog="agent-gateway")
   ap.add_argument("--config", default="/usr/local/etc/agent-gateway/agent-gateway.toml")
   ap.add_argument("-v", "--verbose", action="store_true")
   args = ap.parse_args()
   logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                       format="%(asctime)s %(levelname)s %(name)s: %(message)s")
   cfg = config.load(args.config)
   os.makedirs(cfg.mirrors_dir, exist_ok=True)
   store = Store(os.path.join(cfg.state_dir, "gateway.db"))
   repos = Repos(cfg.repos, cfg.mirrors_dir, cfg.work_dir, cfg.run_as, cfg.agent_home,
                 cfg.agent_gitconfig, (cfg.git_name, cfg.git_email))
   runner = Runner(cfg, store, repos)
   await runner.recover()
   housekeeping = asyncio.create_task(runner.housekeeping())  # noqa: F841 (lives with the loop)
   srv = uvicorn.Server(uvicorn.Config(server.app(runner, cfg), host=cfg.host, port=cfg.port,
                                       log_level="warning"))
   await srv.serve()


if __name__ == "__main__":
   asyncio.run(main())
