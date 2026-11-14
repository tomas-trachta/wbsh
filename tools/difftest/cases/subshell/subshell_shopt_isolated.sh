( shopt -s extglob; shopt -q extglob && echo on-in-sub )
shopt -q extglob && echo leaked || echo off-outside
